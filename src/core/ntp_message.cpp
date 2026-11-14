// SPDX-License-Identifier: MIT
#include "ntp_message.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <pico_time/core/epoch.hpp>
#include <span>

// RFC 4330 §5 item 4 discards LI 0, contradicting its §4 alarm value 3.
// LI 0 through 2 are non-alarm values (RFC 5905 §7.3).
namespace pico_time {
namespace {

constexpr std::size_t BITS_PER_BYTE = 8;

// Header field offsets, in bytes (RFC 5905 §7.3).
constexpr std::size_t FLAGS_OFFSET = 0;  // LI | VN | Mode
constexpr std::size_t STRATUM_OFFSET = 1;
constexpr std::size_t ORIGINATE_OFFSET = 24;  // the echo of our request
constexpr std::size_t TRANSMIT_OFFSET = 40;   // seconds, then fraction

constexpr unsigned MODE_MASK = 0x07U;
constexpr unsigned SERVER_MODE = 0x04U;
constexpr std::uint8_t CLIENT_MODE_FLAGS = 0x1BU;  // LI = 0, VN = 3, Mode = 3
constexpr unsigned LEAP_SHIFT = 6U;
constexpr unsigned LEAP_MASK = 0x03U;
constexpr unsigned LEAP_ALARM_VALUE = 3U;  // server clock not synchronised

// Usable strata are 1 through 15 (RFC 5905 §7.3).
constexpr std::uint8_t MIN_USABLE_STRATUM = 1U;
constexpr std::uint8_t MAX_USABLE_STRATUM = 15U;

// Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01).
constexpr std::uint64_t NTP_DELTA = 2208988800ULL;
// Width of each NTP timestamp half, in bits (RFC 5905 §6).
constexpr unsigned FIELD_BITS = 32U;
constexpr std::uint64_t NTP_ERA_SECONDS = 1ULL << FIELD_BITS;

constexpr std::uint64_t MICROSECONDS_PER_SECOND = 1'000'000ULL;

/// Read a big-endian integer at a compile-time checked offset.
template <std::size_t Offset, typename T>
T read_be(std::span<const std::uint8_t, NTP_MESSAGE_LENGTH> message) {
    T value = 0;
    for (const std::uint8_t byte :
         message.template subspan<Offset, sizeof(T)>()) {
        value = static_cast<T>(value << BITS_PER_BYTE) | static_cast<T>(byte);
    }
    return value;
}

/// Write a big-endian integer at a compile-time checked offset.
template <std::size_t Offset, typename T>
void write_be(std::span<std::uint8_t, NTP_MESSAGE_LENGTH> message, T value) {
    auto field = message.template subspan<Offset, sizeof(T)>();
    for (std::size_t i = field.size(); i-- > 0;) {
        field[i] = static_cast<std::uint8_t>(value);
        value = static_cast<T>(value >> BITS_PER_BYTE);
    }
}

/// Convert a 32-bit NTP fraction to microseconds, at most 999999.
std::uint32_t fraction_to_microseconds(std::uint32_t fraction) {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(fraction) * MICROSECONDS_PER_SECOND) >>
        FIELD_BITS);
}

bool in_plausibility_window(std::uint64_t epoch_seconds) {
    return epoch_seconds >= MIN_EPOCH_SECONDS &&
           epoch_seconds < MAX_EPOCH_SECONDS;
}

// A window narrower than an era admits at most one candidate timestamp.
static_assert(EPOCH_WINDOW_SECONDS < NTP_ERA_SECONDS,
              "the plausibility window must be narrower than an NTP era");

/// Choose era 0 or 1 within the epoch window, or return nullopt (NR-5, VR-6).
std::optional<std::uint64_t> rebase_to_epoch(std::uint32_t seconds_since_1900) {
    const auto field = static_cast<std::uint64_t>(seconds_since_1900);

    // An era-0 field below NTP_DELTA predates 1970 and would underflow.
    if (field >= NTP_DELTA) {
        const std::uint64_t era_0 = field - NTP_DELTA;
        if (in_plausibility_window(era_0)) {
            return era_0;
        }
    }

    const std::uint64_t era_1 = field + NTP_ERA_SECONDS - NTP_DELTA;
    if (in_plausibility_window(era_1)) {
        return era_1;
    }

    return std::nullopt;
}

}  // namespace

std::array<std::uint8_t, NTP_MESSAGE_LENGTH> build_ntp_request(
    std::uint64_t nonce) {
    std::array<std::uint8_t, NTP_MESSAGE_LENGTH> message{};
    message[FLAGS_OFFSET] = CLIENT_MODE_FLAGS;
    write_be<TRANSMIT_OFFSET, std::uint64_t>(message, nonce);
    return message;
}

NtpParse parse_ntp_response(
    std::span<const std::uint8_t, NTP_MESSAGE_LENGTH> message,
    std::uint64_t expected_originate) {
    // The nonce check precedes content so unrelated replies can be dropped
    // (VR-4, VR-7).
    if (read_be<ORIGINATE_OFFSET, std::uint64_t>(message) !=
        expected_originate) {
        return {.reason = NtpReject::ORIGINATE_MISMATCH,
                .sample = std::nullopt};
    }

    const std::uint8_t flags = message[FLAGS_OFFSET];

    // Conversion before masking avoids signed integer promotion of wire bytes.
    const auto flag_bits = static_cast<unsigned>(flags);

    // Mode 4 denotes a server reply (VR-1; RFC 5905 §7.3).
    if ((flag_bits & MODE_MASK) != SERVER_MODE) {
        return {.reason = NtpReject::MODE, .sample = std::nullopt};
    }

    // Leap indicator 3 denotes an unsynchronised clock (VR-2; RFC 5905 §7.3).
    if (((flag_bits >> LEAP_SHIFT) & LEAP_MASK) == LEAP_ALARM_VALUE) {
        return {.reason = NtpReject::LEAP_ALARM, .sample = std::nullopt};
    }

    // Strata 0 and 16 carry a timestamp that must not be trusted (VR-3).
    // Strata 17 through 255 are reserved (RFC 5905 §7.3).
    const std::uint8_t stratum = message[STRATUM_OFFSET];
    if (stratum < MIN_USABLE_STRATUM || stratum > MAX_USABLE_STRATUM) {
        return {.reason = NtpReject::STRATUM, .sample = std::nullopt};
    }

    const auto seconds_since_1900 =
        read_be<TRANSMIT_OFFSET, std::uint32_t>(message);

    // Zero is checked before rebasing, which would place it in 2036 (VR-5).
    if (seconds_since_1900 == 0) {
        return {.reason = NtpReject::TIMESTAMP_ZERO, .sample = std::nullopt};
    }

    // A timestamp outside the window in both candidate eras is rejected (VR-6).
    const std::optional<std::uint64_t> epoch_seconds =
        rebase_to_epoch(seconds_since_1900);
    if (!epoch_seconds.has_value()) {
        return {.reason = NtpReject::TIMESTAMP_IMPLAUSIBLE,
                .sample = std::nullopt};
    }

    constexpr std::size_t FRACTION_OFFSET =
        TRANSMIT_OFFSET + sizeof(std::uint32_t);
    const std::uint32_t microseconds = fraction_to_microseconds(
        read_be<FRACTION_OFFSET, std::uint32_t>(message));

    return {.reason = NtpReject::NONE,
            .sample = NtpSample{.epoch_seconds = *epoch_seconds,
                                .epoch_microseconds = microseconds}};
}

const char* describe(NtpReject reason) {
    switch (reason) {
        case NtpReject::NONE:
            return "accepted";
        case NtpReject::ORIGINATE_MISMATCH:
            return "originate timestamp does not match the request";
        case NtpReject::MODE:
            return "not a server reply";
        case NtpReject::LEAP_ALARM:
            return "server clock unsynchronized (leap alarm)";
        case NtpReject::STRATUM:
            return "unusable stratum";
        case NtpReject::TIMESTAMP_ZERO:
            return "transmit timestamp unset";
        case NtpReject::TIMESTAMP_IMPLAUSIBLE:
            return "timestamp outside the plausible window";
    }
    return "unknown";
}

}  // namespace pico_time
