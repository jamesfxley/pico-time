// SPDX-License-Identifier: MIT

#include "lwip_ntp_transport.hpp"

extern "C" {
#include <lwip/arch.h>
#include <lwip/dns.h>
#include <lwip/err.h>
#include <lwip/ip4_addr.h>
#include <lwip/ip_addr.h>
// misc-include-cleaner does not attribute option macros to opt.h.
#include <lwip/opt.h>  // IWYU pragma: keep
#include <lwip/pbuf.h>
#include <lwip/udp.h>
// The address encoding tests LWIP_IPV4, which misc-include-cleaner attributes
// to the options header.
#include <lwipopts.h>
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

// NO_SYS selects the callback locking model this adapter assumes.
// Comparing against zero avoids a preprocessed 1 == 1 tautology.
// misc-include-cleaner does not attribute the macro to its header.
// NOLINTNEXTLINE(misc-include-cleaner)
static_assert(NO_SYS != 0,
              "pico-time requires the NO_SYS lwIP configuration: its callbacks "
              "assume the cyw43_arch_lwip_begin/end locking model");

constexpr std::uint16_t NTP_PORT = 123;

constexpr std::uint64_t US_PER_MS = 1000;

// NtpAddress is an opaque blob to everything above the seam, which is what
// keeps ip_addr_t out of the policy's headers. This is the one place the two
// spellings meet.
//
// The synchroniser compares NtpAddress bytes to decide whether a datagram came
// from the server it asked. The raw bytes of an ip_addr_t are not used for
// that: lwIP does not document the value of its padding or of the union bytes
// an IPv4 address leaves unused. The encoding below writes only the address
// type and the address into zeroed bytes, so equal endpoints give equal bytes
// in IPv4-only, IPv6-only and dual-stack configurations.
//
// The IPv6 zone is not encoded. With LWIP_IPV6_SCOPES, lwIP gives a received
// link-local source the interface's zone but leaves a DNS result without one,
// so encoding it would make a link-local server's replies never match. The
// request is sent to the DNS result, which carries no zone either.
//
// Layout: the address type, then the 16 address bytes (an IPv4 address fills
// the first four, the rest stay zero).
constexpr std::size_t TYPE_OFFSET = 0;
constexpr std::size_t ADDRESS_OFFSET = TYPE_OFFSET + 1;
constexpr std::size_t ADDRESS_BYTES = 16;
constexpr std::size_t ENCODED_BYTES = ADDRESS_OFFSET + ADDRESS_BYTES;

static_assert(ENCODED_BYTES <= NTP_ADDRESS_BYTES,
              "NtpAddress::opaque is too small for the encoded address; "
              "widen NTP_ADDRESS_BYTES in ntp_transport.hpp");
#if LWIP_IPV4
static_assert(sizeof(ip4_addr_t) <= ADDRESS_BYTES,
              "an IPv4 address does not fit the encoded address bytes");
#endif
#if LWIP_IPV6
static_assert(sizeof(ip6_addr_t::addr) == ADDRESS_BYTES,
              "the IPv6 address words are not the encoded address bytes");
#endif

[[nodiscard]] NtpAddress to_opaque(const ip_addr_t& address) {
    NtpAddress out{};
    const std::span<std::uint8_t> bytes{out.opaque};
    const std::span<std::uint8_t> payload =
        bytes.subspan(ADDRESS_OFFSET, ADDRESS_BYTES);
    out.valid = true;
#if LWIP_IPV6
    if (IP_IS_V6_VAL(address)) {
        const ip6_addr_t* const v6 = ip_2_ip6(&address);
        bytes[TYPE_OFFSET] = IPADDR_TYPE_V6;
        std::memcpy(payload.data(), v6->addr, sizeof(v6->addr));
        return out;
    }
#endif
#if LWIP_IPV4
    const ip4_addr_t* const v4 = ip_2_ip4(&address);
    bytes[TYPE_OFFSET] = IPADDR_TYPE_V4;
    std::memcpy(payload.data(), &v4->addr, sizeof(v4->addr));
#endif
    return out;
}

[[nodiscard]] ip_addr_t from_opaque(const NtpAddress& address) {
    ip_addr_t out{};
    const std::span<const std::uint8_t> bytes{address.opaque};
    const std::span<const std::uint8_t> payload =
        bytes.subspan(ADDRESS_OFFSET, ADDRESS_BYTES);
#if LWIP_IPV6
    if (bytes[TYPE_OFFSET] == IPADDR_TYPE_V6) {
        ip6_addr_t v6{};
        std::memcpy(v6.addr, payload.data(), sizeof(v6.addr));
        ip_addr_copy_from_ip6(out, v6);
        return out;
    }
#endif
#if LWIP_IPV4
    ip4_addr_t v4{};
    std::memcpy(&v4.addr, payload.data(), sizeof(v4.addr));
    ip_addr_copy_from_ip4(out, v4);
#endif
    return out;
}

void on_dns_resolved(const char* /*hostname*/, const ip_addr_t* address,
                     void* arg) {
    auto* observer = static_cast<IResolveObserver*>(arg);
    if (observer == nullptr) {
        return;
    }
    // The callback records under the lock. The mainline sends the request
    // (VR-8).
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
        // A cached result schedules no callback.
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
                // The pbuf is copied before release. The observer span must not
                // outlive this call.
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
        // Disarm before the caller reads the sample to prevent another datagram
        // overwriting its timestamp halves.
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

    // A copy failure returns false before a partially filled request can be
    // sent.
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
    // The 64-bit microsecond source avoids the 49.7-day wrap of
    // to_ms_since_boot().
    return to_us_since_boot(get_absolute_time()) / US_PER_MS;
}

void LwipNtpTransport::wait(std::chrono::milliseconds duration) {
    sleep_ms(static_cast<std::uint32_t>(duration.count()));
}

std::uint64_t LwipNtpTransport::nonce() { return get_rand_64(); }

}  // namespace pico_time
