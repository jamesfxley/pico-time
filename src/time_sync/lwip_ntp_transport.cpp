// SPDX-License-Identifier: MIT
// lwIP glue and nothing else. Every decision about what to do with a datagram,
// how many attempts to make and when a slot may be reused lives above the seam
// in pico_time::NtpSynchronizer, where a host test can drive it.

#include "lwip_ntp_transport.hpp"

extern "C" {
#include <lwip/arch.h>
#include <lwip/dns.h>
#include <lwip/err.h>
#include <lwip/ip_addr.h>
// opt.h supplies the lwIP option macros the static_assert below checks. The
// pragma is load-bearing: include-cleaner does not count a macro expansion as a
// use, so it reports this include as unused.
#include <lwip/opt.h>  // IWYU pragma: keep
#include <lwip/pbuf.h>
#include <lwip/udp.h>
}

#include <pico/cyw43_arch.h>
#include <pico/rand.h>
#include <pico/time.h>
#include <pico/types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <pico_time/ntp/ntp_transport.hpp>
#include <span>

namespace pico_time {
namespace {

// NO_SYS is the lwIP option this file's correctness actually rests on: it
// decides whether the stack runs from the cyw43 background context under
// cyw43_arch_lwip_begin/end, which is the locking model lock()/unlock() and
// every callback here assume. UDP, DNS and IPv4 are not asserted because their
// absence is already a compile or link failure, whereas NO_SYS=0 builds and is
// wrong.
//
// Tested against zero rather than one so the assertion survives preprocessing
// as a comparison rather than as the tautology `1 == 1`, which analysis reports
// as a redundant expression. include-cleaner does not count a macro expansion
// as a use of its header, so it sees no header providing this.
// NOLINTNEXTLINE(misc-include-cleaner)
static_assert(NO_SYS != 0,
              "pico-time requires the NO_SYS lwIP configuration: its callbacks "
              "assume the cyw43_arch_lwip_begin/end locking model");

constexpr std::uint16_t NTP_PORT = 123;

constexpr std::uint64_t US_PER_MS = 1000;

// NtpAddress is an opaque blob to everything above the seam, which is what
// keeps ip_addr_t out of the policy's headers. This is the one place the two
// spellings meet, so this is where the size has to hold.
//
// It is also what keeps the policy's byte comparison of two addresses honest,
// which is the less obvious of its two jobs. An ip_addr_t large enough to fail
// this is a dual-stack one carrying a zone field, which is exactly the
// configuration whose bytes stop being canonical for a given endpoint. So the
// comparison is sound below this bound and refused above it, and widening
// NtpAddress::opaque without reading the sender check in ntp_synchronizer.cpp
// would trade a build error for a silent one.
static_assert(sizeof(ip_addr_t) <= sizeof(NtpAddress::opaque),
              "NtpAddress::opaque is too small for this lwIP configuration's "
              "ip_addr_t; widen it in ntp_transport.hpp");

// std::memcpy rather than a cast through unsigned char*: ip_addr_t is
// trivially copyable, and memcpy between two such objects is the one blessed
// way to reinterpret storage without the cast the guideline refuses
// (CXX-03). std::bit_cast does not apply -- the two types differ in size.
[[nodiscard]] NtpAddress to_opaque(const ip_addr_t& address) {
    NtpAddress out;
    std::memcpy(out.opaque.data(), &address, sizeof(ip_addr_t));
    out.valid = true;
    return out;
}

[[nodiscard]] ip_addr_t from_opaque(const NtpAddress& address) {
    ip_addr_t out{};
    std::memcpy(&out, address.opaque.data(), sizeof(ip_addr_t));
    return out;
}

void on_dns_resolved(const char* /*hostname*/, const ip_addr_t* address,
                     void* arg) {
    auto* observer = static_cast<IResolveObserver*>(arg);
    if (observer == nullptr) {
        return;
    }
    // Runs in the lwIP callback context, holding the lock. Records and returns:
    // the request is sent by the mainline once it observes the address, so no
    // allocation or lock acquisition happens here (VR-8).
    observer->on_resolved(address == nullptr ? NtpAddress{}
                                             : to_opaque(*address));
}

}  // namespace

void LwipNtpTransport::lock() { cyw43_arch_lwip_begin(); }

void LwipNtpTransport::unlock() { cyw43_arch_lwip_end(); }

ResolveStatus LwipNtpTransport::resolve(const char* host, NtpAddress& address,
                                        IResolveObserver& observer) {
    ip_addr_t resolved{};
    const err_t result =
        dns_gethostbyname(host, &resolved, on_dns_resolved, &observer);
    if (result == ERR_OK) {
        // Answered from the cache: the address is already in place and no
        // callback will fire.
        address = to_opaque(resolved);
        return ResolveStatus::RESOLVED;
    }
    if (result == ERR_INPROGRESS) {
        return ResolveStatus::IN_PROGRESS;
    }
    return ResolveStatus::FAILED;
}

bool LwipNtpTransport::open(IDatagramObserver& observer) {
    cyw43_arch_lwip_begin();
    pcb_ = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb_ != nullptr) {
        receiver_ = &observer;
        udp_recv(
            pcb_,
            [](void* arg, udp_pcb* /*pcb*/, pbuf* packet,
               const ip_addr_t* sender, u16_t port) {
                auto* self = static_cast<LwipNtpTransport*>(arg);
                if (self == nullptr || packet == nullptr) {
                    if (packet != nullptr) {
                        pbuf_free(packet);
                    }
                    return;
                }
                if (port != NTP_PORT || self->receiver_ == nullptr) {
                    pbuf_free(packet);
                    return;
                }
                // Copied out before the pbuf is released, because the span
                // handed to the observer must not outlive this call and lwIP
                // requires the callback to free the buffer.
                std::array<std::uint8_t, 64> raw{};
                const u16_t copied =
                    pbuf_copy_partial(packet, raw.data(),
                                      static_cast<u16_t>(std::min<std::size_t>(
                                          raw.size(), packet->tot_len)),
                                      /*offset=*/0);
                pbuf_free(packet);
                self->receiver_->on_datagram(
                    sender == nullptr ? NtpAddress{} : to_opaque(*sender),
                    std::span<const std::uint8_t>(raw.data(), copied));
            },
            this);
    }
    cyw43_arch_lwip_end();
    return pcb_ != nullptr;
}

void LwipNtpTransport::close() {
    cyw43_arch_lwip_begin();
    if (pcb_ != nullptr) {
        // Disarm before the caller reads anything the callback wrote. A second
        // datagram could otherwise overwrite the timestamp halves while the
        // caller reads them (CXX-08.1).
        udp_recv(pcb_, nullptr, nullptr);
        udp_remove(pcb_);
        pcb_ = nullptr;
    }
    receiver_ = nullptr;
    cyw43_arch_lwip_end();
}

bool LwipNtpTransport::send(const NtpAddress& server,
                            std::span<const std::uint8_t> payload) {
    const ip_addr_t destination = from_opaque(server);

    cyw43_arch_lwip_begin();
    pbuf* buffer = pbuf_alloc(PBUF_TRANSPORT,
                              static_cast<u16_t>(payload.size()), PBUF_RAM);
    if (buffer == nullptr) {
        cyw43_arch_lwip_end();
        return false;
    }

    // The pbuf was allocated at exactly this size, so the copy cannot be short;
    // checking anyway keeps the failure a status rather than a half-filled
    // request that reads as a valid one on the wire.
    err_t err =
        pbuf_take(buffer, payload.data(), static_cast<u16_t>(payload.size()));
    if (err == ERR_OK) {
        err = udp_sendto(pcb_, buffer, &destination, NTP_PORT);
    }
    pbuf_free(buffer);
    cyw43_arch_lwip_end();
    return err == ERR_OK;
}

void LwipNtpTransport::poll() { cyw43_arch_poll(); }

std::uint64_t LwipNtpTransport::now_ms() const {
    // From the microsecond counter rather than to_ms_since_boot(), which
    // returns uint32_t and so returns to zero every 49.7 days. The seam
    // declares this monotonic, and a policy is entitled to hold the value
    // across a wait; a counter that steps back under it is a hang, not a
    // late timeout (CXX-08.6).
    return to_us_since_boot(get_absolute_time()) / US_PER_MS;
}

void LwipNtpTransport::wait(std::chrono::milliseconds duration) {
    sleep_ms(static_cast<std::uint32_t>(duration.count()));
}

std::uint64_t LwipNtpTransport::nonce() { return get_rand_64(); }

}  // namespace pico_time
