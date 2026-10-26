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
/// Wall-clock synchronization for the Pico 2 W.
///
/// The board has no battery-backed RTC: the wall clock is kept as an offset
/// from the SDK's monotonic microsecond counter, installed into the newlib
/// _gettimeofday/_settimeofday syscalls so std::chrono::system_clock,
/// time(), and gettimeofday() all reflect it. Before the first
/// synchronization those clocks count up from boot.
///
/// Every entry point below is [[nodiscard]] and reports failure through its
/// return value. The target builds without exceptions, so that value is the
/// only failure channel there is and discarding it discards the failure
/// (AR-1). That value is a pico_util::Result carrying this library's own
/// error vocabulary: the carrier is generic, the taxonomy is not (CXX-07.1).
///
/// Requirement IDs cited here are defined in docs/technical-specification.md.

namespace pico_time {

/// Timeout synchronize_ntp() applies when the caller names none. Ten seconds
/// covers a DNS lookup and a round trip on a congested link without leaving a
/// caller that has no other work blocked for an unbounded time.
inline constexpr auto DEFAULT_SYNC_TIMEOUT = std::chrono::seconds(10);

/// Largest timeout synchronize_ntp() accepts. This call blocks the core it is
/// made on, so the cap is the longest a caller can reasonably be asked to
/// surrender it for; beyond an hour a caller wants its own retry loop rather
/// than one long wait it cannot observe (AR-2).
inline constexpr auto MAX_SYNC_TIMEOUT = std::chrono::hours(1);

/// Synchronize the wall clock via NTP (pool.ntp.org). Blocking; requires
/// cyw43_arch to be initialized and Wi-Fi connected. Makes up to three
/// DNS + NTP attempts, each bounded by `timeout`, and succeeds once the clock
/// has been set — immediately, without network traffic, if it is already
/// synchronized.
///
/// \return a pico_util::Result<void, NtpError>. Its operator bool is explicit,
///         so `if (synchronize_ntp())` reads the outcome and
///         `bool ok = synchronize_ntp();` does not compile.
///
/// Fails without contacting the network with NtpError::INVALID_TIMEOUT if
/// `timeout` is not positive or exceeds MAX_SYNC_TIMEOUT (AR-2), and with
/// NtpError::NO_ATTEMPT_SLOT if every attempt slot is still held by a DNS
/// lookup that has not landed (AR-6) — a transient condition that clears once
/// those lookups call back, and which `retryable()` reports as such so a caller
/// can implement the retry AR-6 prescribes.
///
/// Each reply is validated before it is trusted: server mode, leap indicator,
/// stratum, a per-request nonce echoed in the originate timestamp, and a
/// plausibility window on the timestamp itself. A reply failing any of these
/// is refused rather than applied (VR-1..VR-6), and the refusal is reported as
/// the matching NtpError::REJECTED_* rather than being folded into one bit.
///
/// Worst case (all attempts time out) blocks for roughly
/// 3 * timeout + 2 * 1s inter-attempt delay, so budget accordingly on the
/// calling core.
[[nodiscard]] pico_util::Result<void, NtpError> synchronize_ntp(
    std::chrono::milliseconds timeout = DEFAULT_SYNC_TIMEOUT);

/// True once the wall clock has been set at least once (by synchronize_ntp()
/// or apply_wall_clock_time()). It never becomes false again.
[[nodiscard]] bool is_time_synchronized();

/// Set the wall clock directly from a Unix epoch time obtained elsewhere
/// (external RTC, server response, ...).
///
/// Fails without changing the clock — and without marking it synchronized —
/// with WallClockError::MICROSECONDS_INVALID if `epoch_microseconds` exceeds
/// 999999, and with WallClockError::EPOCH_OUT_OF_RANGE if `epoch_seconds` falls
/// outside [MIN_EPOCH_SECONDS, MAX_EPOCH_SECONDS). The seconds bound is not
/// decoration: it is what stops a caller who passes microseconds-since-epoch
/// here from overflowing the conversion (AR-3), and telling the two apart is
/// what lets that caller find its own mistake.
///
/// **Caller obligation (FR-6).** Call this from mainline context, from one
/// caller, and at most once per second. Those are the terms the offset's
/// publication ring is sized against, and this entry point -- with the
/// settimeofday() shim over it -- is the only way to reach the ring's writer
/// without them holding by construction. Nothing enforces them: a caller that
/// stores in a tight loop can lap the ring inside every attempt of a
/// concurrent read, which then reports EXHAUSTED, so _gettimeofday() fails with
/// EAGAIN. Two writers at once, on two cores or one in an interrupt, are not
/// detected: a read can return a time made of both, and report it as valid.
[[nodiscard]] pico_util::Result<void, WallClockError> apply_wall_clock_time(
    std::uint64_t epoch_seconds, std::uint32_t epoch_microseconds);

}  // namespace pico_time
