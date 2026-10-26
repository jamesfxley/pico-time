// SPDX-License-Identifier: MIT
#pragma once

/// \file lwip_ntp_transport.hpp
/// The lwIP side of the NTP transport seam. Implementation detail (CXX-13.3):
/// it sits beside the source that uses it, is in no FILE_SET, and is never
/// installed. A consumer reaches this library through synchronize_ntp(), and a
/// consumer testing its own code against the seam links
/// pico_time::testing::FakeNtpTransport instead.

#include <chrono>
#include <cstdint>
#include <pico_time/ntp/ntp_transport.hpp>
#include <span>

extern "C" {
#include <lwip/udp.h>
}

namespace pico_time {

/// INtpTransport over lwIP raw UDP and the cyw43 background context.
///
/// Every callback here runs in that background context, which pre-empts the
/// mainline, so each one stores and nothing else (VR-8): it records what
/// happened through the observer, and the mainline does the sending and the
/// tearing down. The single exception is pbuf_free() on a received datagram,
/// which the lwIP API requires the callback to release and which returns to
/// lwIP's own pool.
class LwipNtpTransport final : public INtpTransport {
   public:
    void lock() override;
    void unlock() override;
    [[nodiscard]] ResolveStatus resolve(const char* host, NtpAddress& address,
                                        IResolveObserver& observer) override;
    [[nodiscard]] bool open(IDatagramObserver& observer) override;
    void close() override;
    [[nodiscard]] bool send(const NtpAddress& server,
                            std::span<const std::uint8_t> payload) override;
    void poll() override;
    [[nodiscard]] std::uint64_t now_ms() const override;
    void wait(std::chrono::milliseconds duration) override;
    [[nodiscard]] std::uint64_t nonce() override;

   private:
    udp_pcb* pcb_ = nullptr;
    IDatagramObserver* receiver_ = nullptr;
};

}  // namespace pico_time
