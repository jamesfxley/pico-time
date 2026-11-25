// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <pico_time/ntp/ntp_error.hpp>
#include <pico_time/ntp/ntp_transport.hpp>
#include <pico_util/result/result.hpp>
#include <span>

/// \file ntp_synchronizer.hpp
/// A transport-independent NTP attempt loop and slot pool (AR-6).

namespace pico_time {

/// Attempts made before giving up (NR-3).
inline constexpr std::size_t MAX_REQUEST_ATTEMPTS = 3;

/// Delay between attempts, in milliseconds.
inline constexpr auto RETRY_DELAY = std::chrono::milliseconds(1000);

/// How long the poll loop sleeps between servicing the background context.
inline constexpr auto POLL_DELAY = std::chrono::milliseconds(50);

/// What an accepted reply carried, for the caller that applies it.
struct SynchronizedTime {
    /// Seconds since the Unix epoch, inside the plausibility window (VR-6).
    std::uint64_t epoch_seconds = 0;
    /// Microseconds within that second; never above 999999.
    std::uint32_t epoch_microseconds = 0;
};

/// Runs attempts with a fixed slot pool and no allocation (VR-8).
class NtpSynchronizer {
   public:
    /// \param transport the environment; must outlive this object.
    /// \param server a NUL-terminated server name, used for every attempt.
    NtpSynchronizer(INtpTransport& transport, const char* server)
        : transport_(&transport), server_(server) {}

    /// Run up to MAX_REQUEST_ATTEMPTS exchanges, each bounded by timeout. The
    /// call ends at the first error that retryable() classes as not retryable.
    /// \return the time or last error. NO_ATTEMPT_SLOT returns immediately
    /// (AR-6).
    [[nodiscard]] pico_util::Result<SynchronizedTime, NtpError> synchronize(
        std::chrono::milliseconds timeout);

    /// The number of held slots, including those awaiting abandoned lookups.
    [[nodiscard]] std::size_t slots_in_use() const;

   private:
    /// State retained until both the caller and the resolution callback finish.
    /// Plain fields rely on single-core callbacks with NO_SYS enabled.
    /// The lwIP adapter asserts NO_SYS.
    class Slot final : public IResolveObserver, public IDatagramObserver {
       public:
        void on_resolved(const NtpAddress& address) override;
        void on_datagram(const NtpAddress& sender,
                         std::span<const std::uint8_t> datagram) override;

        /// Reset fields individually. The bases are non-copyable, as an
        /// interface should be.
        void reset();

        /// The synchronizer owning this slot, for the lock and the nonce.
        NtpSynchronizer* owner = nullptr;
        NtpAddress server_address{};
        std::uint64_t nonce = 0;
        /// Set by on_resolved once an address is in place.
        bool address_ready = false;
        /// Set when an attempt has finished, successfully or not.
        bool complete = false;
        /// Why it finished, NONE while it has not.
        NtpError error = NtpError::NONE;
        SynchronizedTime sample{};
        /// A resolution is outstanding and will call back.
        bool resolve_pending = false;
        /// The caller gave up; the callback releases this slot.
        bool abandoned = false;
        /// Claimed by an attempt.
        bool in_use = false;
    };

    [[nodiscard]] Slot* claim_slot();
    static void release_slot(Slot& slot);
    [[nodiscard]] NtpError run_attempt(Slot& slot,
                                       std::chrono::milliseconds timeout);
    /// Poll until ready or budget_ms milliseconds have elapsed since start_ms.
    [[nodiscard]] bool poll_until(const Slot& slot, std::uint64_t start_ms,
                                  std::uint64_t budget_ms,
                                  bool (*ready)(const Slot&));

    INtpTransport* transport_;
    const char* server_;
    std::array<Slot, MAX_REQUEST_ATTEMPTS> slots_{};
};

}  // namespace pico_time
