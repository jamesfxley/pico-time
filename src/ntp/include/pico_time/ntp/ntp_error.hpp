// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

/// \file ntp_error.hpp
/// Why a synchronization attempt did not set the clock.
///
/// This library owns the taxonomy of the failures it produces and re-exports
/// nobody else's (CXX-07.1). The carrier is pico_util::Result, which is
/// generic; the vocabulary below is not.
///
/// Every cause the client can distinguish is here, rather than being rendered
/// into a log line and dropped. AR-6 calls slot exhaustion transient and
/// prescribes a retry, which a caller cannot implement without being able to
/// tell it from a DNS failure or a server that refused its own reply.
///
/// Requirement IDs cited here are defined in docs/technical-specification.md.

namespace pico_time {

/// A synchronization failure. NONE is the success state, which is what makes
/// this usable as the error type of a pico_util::Result, which treats the
/// value-initialised error as success.
enum class NtpError : std::uint8_t {
    NONE,  ///< Succeeded.

    // Refused before any network traffic.
    INVALID_TIMEOUT,  ///< Not positive, or above MAX_SYNC_TIMEOUT (AR-2).
    NO_ATTEMPT_SLOT,  ///< Every slot held by a lookup that has not landed
                      ///< (AR-6). Transient; retryable.

    // The exchange did not complete.
    DNS_FAILED,     ///< The server name did not resolve.
    SOCKET_FAILED,  ///< No UDP endpoint could be opened.
    /// The request could not be put on the wire. Covers a buffer that could
    /// not be allocated as well as a send that was refused: INtpTransport's
    /// send() reports a bool, so the two are not distinguishable at this seam.
    SEND_FAILED,
    TIMED_OUT,  ///< No acceptable reply inside the caller's budget.

    // A reply arrived and was refused. These are the NtpReject verdicts that
    // end an attempt; ORIGINATE_MISMATCH is not among them because a datagram
    // that has not proved it belongs to this exchange is dropped rather than
    // allowed to end an attempt that would otherwise have succeeded (VR-7).
    REJECTED_MODE,            ///< Not a server reply.
    REJECTED_LEAP_ALARM,      ///< The server's own clock is unset.
    REJECTED_STRATUM,         ///< Kiss-o'-Death, unsynchronized, reserved.
    REJECTED_TIMESTAMP_ZERO,  ///< Transmit timestamp unset.
    REJECTED_TIMESTAMP_IMPLAUSIBLE,  ///< Outside the window firmware can see.

    /// A reply was accepted but the clock refused it (AR-3).
    CLOCK_REFUSED,
};

/// Whether retrying could succeed without anything else changing.
///
/// AR-6 is the case this exists for: slot exhaustion clears on its own once
/// the outstanding lookups call back, so a caller that treats it like a DNS
/// failure gives up on a condition that was about to resolve. A timeout is
/// retryable for the same reason a congested link is. A refused reply is not:
/// the server has reported its own state and will report it again.
///
/// SOCKET_FAILED and SEND_FAILED are on this side of the line because neither
/// is a server reporting anything. They are the lwIP pools momentarily full --
/// MEMP_NUM_UDP_PCB and MEM_SIZE respectively -- which is what a concurrent
/// transfer does to them, and it clears in milliseconds.
[[nodiscard]] constexpr bool retryable(NtpError error) {
    return error == NtpError::NO_ATTEMPT_SLOT || error == NtpError::TIMED_OUT ||
           error == NtpError::DNS_FAILED || error == NtpError::SOCKET_FAILED ||
           error == NtpError::SEND_FAILED;
}

/// A stable short name, for a caller's log line. Never null.
[[nodiscard]] const char* describe(NtpError error);

/// Why the wall clock refused a time supplied directly (AR-3).
enum class WallClockError : std::uint8_t {
    NONE,                  ///< Succeeded.
    MICROSECONDS_INVALID,  ///< Above 999999.
    EPOCH_OUT_OF_RANGE,    ///< Outside [MIN_EPOCH_SECONDS, MAX_EPOCH_SECONDS).
};

/// A stable short name, for a caller's log line. Never null.
[[nodiscard]] const char* describe(WallClockError error);

}  // namespace pico_time
