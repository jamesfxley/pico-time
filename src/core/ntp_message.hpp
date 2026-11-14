// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

/// \file ntp_message.hpp
/// SNTP validation over a fixed 48-byte buffer (VR-1..VR-6).

namespace pico_time {

/// Size of the fixed NTP header, in bytes.
inline constexpr std::size_t NTP_MESSAGE_LENGTH = 48;

/// Rejection reasons in check order. The nonce is checked first (VR-4, VR-7).
enum class NtpReject : std::uint8_t {
    NONE,                ///< Accepted.
    ORIGINATE_MISMATCH,  ///< Not a reply to the request we sent.
    MODE,                ///< Not a server reply (mode != 4).
    LEAP_ALARM,          ///< Leap indicator 3: the server's own clock is unset.
    STRATUM,             ///< Kiss-o'-Death, unsynchronised, or reserved.
    TIMESTAMP_ZERO,      ///< Transmit timestamp unset.
    TIMESTAMP_IMPLAUSIBLE  ///< Outside the plausibility window.
};

/// A time sample returned by a successful parse.
struct NtpSample {
    std::uint64_t epoch_seconds = 0;
    std::uint32_t epoch_microseconds = 0;
};

/// A parse result. sample is engaged if and only if reason is NONE.
struct NtpParse {
    NtpReject reason = NtpReject::NONE;
    std::optional<NtpSample> sample;
};

/// Build a mode-3 request with nonce in the transmit field (VR-4).
/// The server echoes that field as the originate timestamp (RFC 5905 §8).
[[nodiscard]] std::array<std::uint8_t, NTP_MESSAGE_LENGTH> build_ntp_request(
    std::uint64_t nonce);

/// Validate a reply and convert its transmit time to Unix epoch (VR-1..VR-6).
/// A response must fall inside the epoch.hpp plausibility window (VR-6).
/// expected_originate is the nonce passed to build_ntp_request().
[[nodiscard]] NtpParse parse_ntp_response(
    std::span<const std::uint8_t, NTP_MESSAGE_LENGTH> message,
    std::uint64_t expected_originate);

/// A stable short name for a rejection, for the caller's log line. Never null.
[[nodiscard]] const char* describe(NtpReject reason);

}  // namespace pico_time
