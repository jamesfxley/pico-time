// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

/// \file ntp_message.hpp
/// The SNTP wire format, as pure functions over a fixed 48-byte buffer.
///
/// This is the trust boundary (CXX-12.1): everything an NTP server sends
/// arrives here, and nothing downstream re-validates it. Keeping it free of
/// lwIP and of the Pico SDK is deliberate -- it is what lets the validation
/// rules be tested at their boundaries and fuzzed on a host (VR-1..VR-6,
/// EN-04.6),
/// which is the only way the rules below are checkable at all.
///
/// Requirement IDs cited here are defined in docs/technical-specification.md.

namespace pico_time {

/// Every NTP message is exactly this long. A std::span of static extent
/// carries it, so no length arithmetic exists to get wrong (CXX-12.3).
inline constexpr std::size_t NTP_MESSAGE_LENGTH = 48;

/// Why a response was refused. Ordered as the checks run, so the value also
/// says how far the message got. ORIGINATE_MISMATCH comes first because
/// authentication comes first: it is the only verdict an unauthenticated
/// datagram can reach, which is what lets the caller distinguish "not ours"
/// from "ours and wrong" (VR-4, VR-7).
enum class NtpReject : std::uint8_t {
    NONE,                ///< Accepted.
    ORIGINATE_MISMATCH,  ///< Not a reply to the request we sent.
    MODE,                ///< Not a server reply (mode != 4).
    LEAP_ALARM,          ///< Leap indicator 3: the server's own clock is unset.
    STRATUM,             ///< Kiss-o'-Death, unsynchronized, or reserved.
    TIMESTAMP_ZERO,      ///< Transmit timestamp unset.
    TIMESTAMP_IMPLAUSIBLE  ///< Outside the window firmware can legitimately
                           ///< see.
};

/// A validated instant. Reachable only through a parse that accepted, so a
/// value of this type is by construction one the checks passed (CXX-12.2).
struct NtpSample {
    std::uint64_t epoch_seconds = 0;
    std::uint32_t epoch_microseconds = 0;
};

/// The outcome of a parse. `sample` is engaged if and only if `reason` is
/// NtpReject::NONE: a refused packet yields no timestamp at all, so a caller
/// cannot reach one by forgetting to check the reason.
struct NtpParse {
    NtpReject reason = NtpReject::NONE;
    std::optional<NtpSample> sample;
};

// The plausibility window a response must fall inside (VR-6) is the same
// window apply_wall_clock_time() enforces, and it is defined once in
// core/epoch.hpp as MIN_EPOCH_SECONDS / MAX_EPOCH_SECONDS. A second copy here
// would be two records of one fact, free to drift apart silently.

/// Build a mode-3 client request carrying `nonce` as its transmit timestamp.
///
/// The nonce is the anti-replay token (VR-4). RFC 4330 §6 has the server echo
/// the request's transmit timestamp back in the reply's originate field, and
/// in unauthenticated SNTP that echo is the only evidence a reply belongs to
/// this request rather than to a captured earlier one.
[[nodiscard]] std::array<std::uint8_t, NTP_MESSAGE_LENGTH> build_ntp_request(
    std::uint64_t nonce);

/// Validate a server reply and convert its transmit timestamp to Unix epoch.
///
/// `expected_originate` is the nonce passed to build_ntp_request() for the
/// request this reply answers. Checks run in the order of NtpReject and every
/// one of them can refuse; see docs/technical-specification.md VR-1..VR-6.
[[nodiscard]] NtpParse parse_ntp_response(
    std::span<const std::uint8_t, NTP_MESSAGE_LENGTH> message,
    std::uint64_t expected_originate);

/// A stable short name for a rejection, for the caller's log line. Never null.
[[nodiscard]] const char* describe(NtpReject reason);

}  // namespace pico_time
