// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
// The window bounds apply_wall_clock_time is stated against.
#include <pico_time/core/epoch.hpp>  // IWYU pragma: export
// The error this API reports.
#include <pico_time/ntp/ntp_error.hpp>  // IWYU pragma: export
#include <pico_util/result/result.hpp>

/// \file time_sync.hpp
/// Wall-clock synchronisation with validated NTP replies (VR-1..VR-6).
/// Newlib hooks expose the shared clock (AR-4).
/// A consumer must not define settimeofday(), _settimeofday() or
/// _gettimeofday(). Return values are the only failure channel on this
/// exception-free target (AR-1).

namespace pico_time {

/// Default per-attempt timeout, in seconds.
inline constexpr auto DEFAULT_SYNC_TIMEOUT = std::chrono::seconds(10);

/// Maximum per-attempt timeout, in hours (AR-2).
inline constexpr auto MAX_SYNC_TIMEOUT = std::chrono::hours(1);

/// Block while setting time from pool.ntp.org. This needs cyw43_arch
/// initialised and Wi-Fi connected.
/// Invalid timeouts or slot exhaustion return before network use (AR-2, AR-6).
[[nodiscard]] pico_util::Result<void, NtpError> synchronize_ntp(
    std::chrono::milliseconds timeout = DEFAULT_SYNC_TIMEOUT);

/// True after the first successful NTP or manual set. Never becomes false.
[[nodiscard]] bool is_time_synchronized();

/// Apply epoch seconds and microseconds, or return a validation error (AR-3).
/// Call from one mainline writer at most once per second. Nothing enforces
/// this, and concurrent writers can produce an undetected torn value (FR-6).
[[nodiscard]] pico_util::Result<void, WallClockError> apply_wall_clock_time(
    std::uint64_t epoch_seconds, std::uint32_t epoch_microseconds);

}  // namespace pico_time
