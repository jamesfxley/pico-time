// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

/// \file ntp_error.hpp
/// Errors returned to the caller of a synchronisation attempt (AR-6).

namespace pico_time {

/// A synchronisation failure. Value-initialised NONE denotes success.
enum class NtpError : std::uint8_t {
    NONE,  ///< Succeeded.

    // Errors returned before any network traffic.
    INVALID_TIMEOUT,  ///< Not positive, or above MAX_SYNC_TIMEOUT (AR-2).
    NO_ATTEMPT_SLOT,  ///< Pending lookups hold all slots. Retryable (AR-6).

    // The exchange did not complete.
    DNS_FAILED,     ///< The server name did not resolve.
    SOCKET_FAILED,  ///< No UDP endpoint could be opened.
    SEND_FAILED,    ///< Sending or buffer allocation failed.
    TIMED_OUT,      ///< No acceptable reply inside the caller's budget.

    // Rejected replies end the attempt. Nonce mismatches are dropped (VR-7).
    REJECTED_MODE,            ///< Not a server reply.
    REJECTED_LEAP_ALARM,      ///< The server's own clock is unset.
    REJECTED_STRATUM,         ///< Kiss-o'-Death, unsynchronised, reserved.
    REJECTED_TIMESTAMP_ZERO,  ///< Transmit timestamp unset.
    REJECTED_TIMESTAMP_IMPLAUSIBLE,  ///< Outside the plausibility window.

    CLOCK_REFUSED,  ///< The clock rejected an accepted reply (AR-3).
};

/// Whether retrying could succeed without another change (AR-6).
/// Socket and send failures can reflect lwIP resource exhaustion. Rejections
/// report server state.
[[nodiscard]] constexpr bool retryable(NtpError error) {
    return error == NtpError::NO_ATTEMPT_SLOT || error == NtpError::TIMED_OUT ||
           error == NtpError::DNS_FAILED || error == NtpError::SOCKET_FAILED ||
           error == NtpError::SEND_FAILED;
}

/// A stable short name, for a caller's log line. Never null.
[[nodiscard]] const char* describe(NtpError error);

/// Errors from setting the wall clock directly (AR-3).
enum class WallClockError : std::uint8_t {
    NONE,                  ///< Succeeded.
    MICROSECONDS_INVALID,  ///< Above 999999.
    EPOCH_OUT_OF_RANGE,    ///< Outside [MIN_EPOCH_SECONDS, MAX_EPOCH_SECONDS).
};

/// A stable short name, for a caller's log line. Never null.
[[nodiscard]] const char* describe(WallClockError error);

}  // namespace pico_time
