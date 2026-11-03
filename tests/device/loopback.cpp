// SPDX-License-Identifier: MIT
#include "loopback.hpp"

// Target definitions can supply every option; otherwise load lwIP defaults.
#if !defined(LWIP_NETIF_LOOPBACK) || !defined(LWIP_HAVE_LOOPIF) || \
    !defined(LWIP_NETIF_LOOPBACK_MULTITHREADING) ||                \
    !defined(DNS_LOCAL_HOSTLIST)
#include <lwip/opt.h>
#endif

#if !LWIP_NETIF_LOOPBACK
LoopbackResult check_loopback_replies() {
    return {.detail = "missing LWIP_NETIF_LOOPBACK"};
}
#elif !LWIP_HAVE_LOOPIF
LoopbackResult check_loopback_replies() {
    return {.detail = "missing LWIP_HAVE_LOOPIF"};
}
#elif LWIP_NETIF_LOOPBACK_MULTITHREADING
LoopbackResult check_loopback_replies() {
    return {.detail = "requires LWIP_NETIF_LOOPBACK_MULTITHREADING=0"};
}
#elif !DNS_LOCAL_HOSTLIST
LoopbackResult check_loopback_replies() {
    return {.detail = "missing DNS_LOCAL_HOSTLIST"};
}
#else

#include <hardware/timer.h>
#include <lwip/dns.h>
#include <lwip/err.h>
#include <lwip/ip.h>
#include <lwip/ip_addr.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#include <pico/async_context.h>
#include <pico/cyw43_arch.h>
#include <sys/_timeval.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <pico_time/core/epoch.hpp>
#include <pico_time/time_sync/time_sync.hpp>
#include <pico_util/result/result.hpp>
#include <span>
#include <string_view>

namespace {

constexpr std::size_t MESSAGE_SIZE = 48;
constexpr std::size_t ORIGINATE_OFFSET = 24;
constexpr std::size_t TRANSMIT_OFFSET = 40;
constexpr std::uint16_t NTP_PORT = 123;
constexpr std::uint16_t OTHER_PORT = NTP_PORT + 1;
constexpr std::uint32_t WORKER_INTERVAL_MS = 20;
constexpr std::int64_t US_PER_SECOND = 1'000'000;
constexpr std::uint64_t REPLY_SECONDS = pico_time::MIN_EPOCH_SECONDS + 86400;
constexpr std::uint64_t NTP_DELTA = 2208988800ULL;
constexpr std::array<std::string_view, 4> FACETS{"wrong-source-port",
                                                 "too-short", "wrong-originate",
                                                 "wrong-source-address"};

struct Peer {
    udp_pcb* server = nullptr;
    udp_pcb* other_port = nullptr;
    udp_pcb* client_pcb = nullptr;
    udp_recv_fn adapter_recv = nullptr;
    void* adapter_argument = nullptr;
    std::size_t callbacks = 0;
    std::size_t forwarded = 0;
    ip_addr_t observed_sender{};
    std::uint16_t observed_port = 0;
    std::uint16_t observed_length = 0;
    bool restored = false;
    ip_addr_t loopback{};
    ip_addr_t client{};
    std::uint16_t client_port = 0;
    std::array<std::uint8_t, MESSAGE_SIZE> reply{};
    std::size_t requests = 0;
    std::size_t facet = 0;
    bool checking = false;
    bool valid_sent = false;
    std::string_view failure;
    async_at_time_worker_t worker{};
};

void fail(Peer& state, std::string_view reason) {
    if (state.failure.empty()) {
        state.failure = reason;
    }
}

void capture_request(void* argument, udp_pcb* /*pcb*/, pbuf* packet,
                     const ip_addr_t* sender, std::uint16_t port) {
    auto& state = *static_cast<Peer*>(argument);
    if (packet == nullptr) {
        fail(state, "request buffer missing");
        return;
    }
    ++state.requests;
    if (state.requests == 1 && sender != nullptr &&
        packet->tot_len == MESSAGE_SIZE) {
        std::array<std::uint8_t, MESSAGE_SIZE> request{};
        const auto copied =
            pbuf_copy_partial(packet, request.data(), MESSAGE_SIZE, 0);
        if (copied != MESSAGE_SIZE) {
            fail(state, "request copy incomplete");
        }
        state.client = *sender;
        state.client_port = port;
        state.reply[0] = 0x24;  // Version 4, server mode, no leap alarm.
        state.reply[1] = 1;
        const auto nonce = std::span{request}.subspan<TRANSMIT_OFFSET, 8>();
        std::ranges::copy(nonce, state.reply.begin() + ORIGINATE_OFFSET);
        auto seconds = static_cast<std::uint32_t>(REPLY_SECONDS + NTP_DELTA);
        auto transmit = std::span{state.reply}.subspan<TRANSMIT_OFFSET, 4>();
        for (std::size_t index = transmit.size(); index-- > 0;) {
            transmit[index] = static_cast<std::uint8_t>(seconds);
            seconds >>= 8U;
        }
    } else if (state.checking) {
        fail(state, FACETS[state.facet]);
    } else {
        fail(state, "unexpected request: attempt retried or request malformed");
    }
    pbuf_free(packet);
}

// Installed only on the request's real client PCB. Record metadata before
// forwarding: the adapter owns/frees packet and sender can point into it.
void observe_adapter(void* argument, udp_pcb* pcb, pbuf* packet,
                     const ip_addr_t* sender, std::uint16_t port) {
    auto& state = *static_cast<Peer*>(argument);
    ++state.callbacks;
    state.observed_sender = sender == nullptr ? ip_addr_t{} : *sender;
    state.observed_port = port;
    state.observed_length = packet == nullptr ? 0 : packet->tot_len;
    state.adapter_recv(state.adapter_argument, pcb, packet, sender, port);
    ++state.forwarded;
}

[[nodiscard]] bool matches_request(const udp_pcb& pcb, const Peer& state) {
    // sendto leaves this adapter's PCB unconnected. Its wildcard local address
    // must accept the request source, and its remote binding must stay empty.
    return pcb.local_port == state.client_port &&
           (ip_addr_isany(&pcb.local_ip) ||
            ip_addr_cmp(&pcb.local_ip, &state.client)) &&
           !udp_is_flag_set(&pcb, UDP_FLAGS_CONNECTED) &&
           pcb.remote_port == 0 && ip_addr_isany(&pcb.remote_ip);
}

[[nodiscard]] bool install_observer(Peer& state) {
    for (auto* pcb = udp_pcbs; pcb != nullptr; pcb = pcb->next) {
        if (pcb != state.server && pcb != state.other_port &&
            matches_request(*pcb, state) && pcb->recv != nullptr) {
            if (state.client_pcb != nullptr) {
                fail(state, "ambiguous client UDP binding");
                return false;
            }
            state.client_pcb = pcb;
        }
    }
    if (state.client_pcb == nullptr) {
        fail(state, "request's client UDP callback not found");
        return false;
    }
    state.adapter_recv = state.client_pcb->recv;
    state.adapter_argument = state.client_pcb->recv_arg;
    udp_recv(state.client_pcb, observe_adapter, &state);
    return true;
}

[[nodiscard]] bool attempt_open(const Peer& state) {
    // Enumerate live PCBs before dereferencing: the adapter can close/remove
    // its endpoint if a bad datagram completed the attempt.
    for (const udp_pcb* pcb = udp_pcbs; pcb != nullptr; pcb = pcb->next) {
        if (pcb == state.client_pcb && matches_request(*pcb, state) &&
            pcb->recv == observe_adapter && pcb->recv_arg == &state) {
            return true;
        }
    }
    return false;
}

void restore_observer(Peer& state) {
    if (attempt_open(state)) {
        udp_recv(state.client_pcb, state.adapter_recv, state.adapter_argument);
        state.restored = true;
    }
}

[[nodiscard]] bool delivered_to_adapter(const Peer& state,
                                        std::size_t previous) {
    ip_addr_t expected_sender = state.loopback;
    if (state.facet == 3) {
        IP_ADDR4(&expected_sender, 127, 0, 0, 2);
    }
    const auto expected_port = state.facet == 0 ? OTHER_PORT : NTP_PORT;
    const auto expected_length =
        state.facet == 1 ? MESSAGE_SIZE - 1 : MESSAGE_SIZE;
    return state.callbacks == previous + 1 &&
           state.forwarded == state.callbacks &&
           ip_addr_cmp(&state.observed_sender, &expected_sender) &&
           state.observed_port == expected_port &&
           state.observed_length == expected_length;
}

[[nodiscard]] bool send_reply(Peer& state, std::size_t facet) {
    auto reply = state.reply;
    if (facet == 2) {
        reply[ORIGINATE_OFFSET] ^= 1U;
    }
    const auto length = static_cast<std::uint16_t>(facet == 1 ? MESSAGE_SIZE - 1
                                                              : MESSAGE_SIZE);
    pbuf* packet = pbuf_alloc(PBUF_TRANSPORT, length, PBUF_RAM);
    if (packet == nullptr) {
        return false;
    }
    err_t status = pbuf_take(packet, reply.data(), length);
    if (status == ERR_OK && facet == 3) {
        // This raw UDP API sends a real IP datagram with an explicit source.
        // The loopif need not own that source alias; no observer is faked.
        ip_addr_t unrelated{};
        IP_ADDR4(&unrelated, 127, 0, 0, 2);
        auto* route = ip_route(&state.loopback, &state.client);
        if (route == nullptr) {
            status = ERR_RTE;
        } else {
            status = udp_sendto_if_src(state.server, packet, &state.client,
                                       state.client_port, route, &unrelated);
        }
    } else if (status == ERR_OK) {
        status = udp_sendto(facet == 0 ? state.other_port : state.server,
                            packet, &state.client, state.client_port);
    }
    pbuf_free(packet);
    return status == ERR_OK;
}

void advance_peer(Peer& state) {
    if (state.requests == 0 || !state.failure.empty() || state.valid_sent) {
        return;
    }
    if (state.checking) {
        if (pico_time::is_time_synchronized() || !attempt_open(state)) {
            fail(state, FACETS[state.facet]);
        } else {
            ++state.facet;
            state.checking = false;
        }
        return;
    }
    if (state.client_pcb == nullptr && !install_observer(state)) {
        return;
    }
    const auto received = state.callbacks;
    if (!send_reply(state, state.facet)) {
        fail(state, "responder could not send datagram");
        return;
    }
    netif_poll_all();
    if (!delivered_to_adapter(state, received)) {
        fail(state, "adapter callback count or datagram metadata mismatch");
        return;
    }
    if (state.facet == FACETS.size()) {
        state.valid_sent = true;
        restore_observer(state);
        return;
    }
    state.checking = true;
    if (pico_time::is_time_synchronized() || !attempt_open(state)) {
        fail(state, FACETS[state.facet]);
    }
}

void service_peer(async_context_t* context, async_at_time_worker_t* worker) {
    auto& state = *static_cast<Peer*>(worker->user_data);
    netif_poll_all();
    advance_peer(state);
    if (!async_context_add_at_time_worker_in_ms(context, worker,
                                                WORKER_INTERVAL_MS)) {
        fail(state, "could not reschedule loopback worker");
    }
}

[[nodiscard]] bool start_peer(Peer& peer, async_context_t* context) {
    peer = Peer{};
    IP_ADDR4(&peer.loopback, 127, 0, 0, 1);
    ip_addr_t resolved{};
    // Confirm the selected configuration actually maps the hardcoded server.
    if (dns_gethostbyname("pool.ntp.org", &resolved, nullptr, nullptr) !=
            ERR_OK ||
        !ip_addr_cmp(&resolved, &peer.loopback)) {
        fail(peer, "DNS_LOCAL_HOSTLIST must map pool.ntp.org to 127.0.0.1");
        return false;
    }
    peer.server = udp_new();
    peer.other_port = udp_new();
    if (peer.server == nullptr || peer.other_port == nullptr ||
        udp_bind(peer.server, &peer.loopback, NTP_PORT) != ERR_OK ||
        udp_bind(peer.other_port, &peer.loopback, OTHER_PORT) != ERR_OK) {
        fail(peer, "could not bind responder UDP endpoints");
        return false;
    }
    udp_recv(peer.server, capture_request, &peer);
    peer.worker.do_work = service_peer;
    peer.worker.user_data = &peer;
    if (!async_context_add_at_time_worker_in_ms(context, &peer.worker,
                                                WORKER_INTERVAL_MS)) {
        fail(peer, "could not register loopback worker");
        return false;
    }
    return true;
}

void stop_peer(Peer& peer, async_context_t* context) {
    (void)async_context_remove_at_time_worker(context, &peer.worker);
    restore_observer(peer);
    if (peer.server != nullptr) {
        udp_remove(peer.server);
        peer.server = nullptr;
    }
    if (peer.other_port != nullptr) {
        udp_remove(peer.other_port);
        peer.other_port = nullptr;
    }
}

}  // namespace

LoopbackResult check_loopback_replies() {
    if (pico_time::is_time_synchronized()) {
        return {.detail = "clock already synchronized before VR-7"};
    }
    if (cyw43_arch_init() != 0) {
        return {.detail = "CYW43/lwIP initialization failed"};
    }
    // Static storage outlives callbacks. The context lock protects all access.
    static Peer peer;
    auto* context = cyw43_arch_async_context();
    cyw43_arch_lwip_begin();
    const bool started = start_peer(peer, context);
    cyw43_arch_lwip_end();
    const auto before = static_cast<std::int64_t>(time_us_64());
    const bool synchronized =
        started &&
        pico_time::synchronize_ntp(std::chrono::milliseconds{500}).ok();
    const auto after = static_cast<std::int64_t>(time_us_64());
    timeval reading{};
    const auto read_before = static_cast<std::int64_t>(time_us_64());
    const int read_status = gettimeofday(&reading, nullptr);
    const auto read_after = static_cast<std::int64_t>(time_us_64());
    cyw43_arch_lwip_begin();
    if (peer.checking &&
        (pico_time::is_time_synchronized() || !attempt_open(peer))) {
        fail(peer, FACETS[peer.facet]);
    }
    stop_peer(peer, context);
    const auto failure = peer.failure;
    const bool delivered = peer.valid_sent && peer.restored &&
                           peer.facet == FACETS.size() && peer.requests == 1 &&
                           peer.callbacks == FACETS.size() + 1 &&
                           peer.forwarded == peer.callbacks;
    cyw43_arch_lwip_end();
    cyw43_arch_deinit();
    if (!failure.empty()) {
        return {.detail = failure};
    }
    const auto expected =
        static_cast<std::int64_t>(REPLY_SECONDS) * US_PER_SECOND;
    const auto observed =
        (static_cast<std::int64_t>(reading.tv_sec) * US_PER_SECOND) +
        reading.tv_usec;
    const bool matches = read_status == 0 && reading.tv_usec >= 0 &&
                         reading.tv_usec < US_PER_SECOND &&
                         observed >= expected + read_before - after &&
                         observed <= expected + read_after - before;
    return {
        .passed = synchronized && delivered && matches &&
                  pico_time::is_time_synchronized(),
        .detail = synchronized && delivered && matches
                      ? "all unrelated datagrams dropped; valid reply applied"
                      : "valid reply, single attempt or clock bracket failed"};
}
#endif
