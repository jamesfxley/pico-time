// SPDX-License-Identifier: MIT
#pragma once

/// \file lwip_ntp_transport.hpp
/// The internal lwIP transport adapter. This header is not installed.

#include <chrono>
#include <cstdint>
#include <pico_time/ntp/ntp_transport.hpp>
#include <span>

extern "C" {
#include <lwip/udp.h>
}

namespace pico_time {

/// Raw UDP over the cyw43 background context. Callbacks record outcomes (VR-8).
/// Received pbufs are released in the callback as required by lwIP.
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
