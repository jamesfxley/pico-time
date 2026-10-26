// SPDX-License-Identifier: MIT
#include "ntp_message.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <pico_time/core/epoch.hpp>
#include <span>

// RFC 4330 §5 item 4's LI-zero rejection conflicts with §4's alarm LI=3.
// The client follows §4: LI 0 through 2 are accepted non-alarm values.
namespace pico_time {
namespace {

constexpr std::size_t BITS_PER_BYTE = 8;

// Field offsets, RFC 4330 §4 ("NTP Message Format").
constexpr std::size_t FLAGS_OFFSET = 0;  // LI | VN | Mode
constexpr std::size_t STRATUM_OFFSET = 1;
constexpr std::size_t ORIGINATE_OFFSET = 24;  // the echo of our request
constexpr std::size_t TRANSMIT_OFFSET = 40;   // seconds, then fraction

constexpr unsigned MODE_MASK = 0x07U;
constexpr unsigned SERVER_MODE = 0x04U;
constexpr std::uint8_t CLIENT_MODE_FLAGS = 0x1BU;  // LI = 0, VN = 3, Mode = 3
constexpr unsigned LEAP_SHIFT = 6U;
constexpr unsigned LEAP_MASK = 0x03U;
constexpr unsigned LEAP_ALARM_VALUE = 3U;  // server clock not synchronized

// RFC 4330 §4: 0 is Kiss-o'-Death, 1 primary, 2-15 secondary, 16-255
// reserved. RFC 5905 §7.3 calls 16 unsynchronized. Only 1-15 is usable.
constexpr std::uint8_t MIN_USABLE_STRATUM = 1U;
constexpr std::uint8_t MAX_USABLE_STRATUM = 15U;

// Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01).
constexpr std::uint64_t NTP_DELTA = 2208988800ULL;
// Width of both 32-bit halves of an NTP timestamp. The seconds half wraps on
// 2036-02-07 (end of era 0); the fraction half is a binary fraction of a
// second, so dividing by 2^FIELD_BITS is a shift.
constexpr unsigned FIELD_BITS = 32U;
constexpr std::uint64_t NTP_ERA_SECONDS = 1ULL << FIELD_BITS;

constexpr std::uint64_t MICROSECONDS_PER_SECOND = 1'000'000ULL;

/// Read a big-endian integer from a compile-time offset.
///
/// subspan<Offset, sizeof(T)> on a static-extent span is bounds-checked by the
/// compiler, so a read past the end of the message is a build error rather
/// than a runtime one (CXX-12.3).
template <std::size_t Offset, typename T>
T read_be(std::span<const std::uint8_t, NTP_MESSAGE_LENGTH> message) {
    T value = 0;
    for (const std::uint8_t byte :
         message.template subspan<Offset, sizeof(T)>()) {
        value = static_cast<T>(value << BITS_PER_BYTE) | static_cast<T>(byte);
    }
    return value;
}

/// Write a big-endian integer at a compile-time offset. Same bound as above.
template <std::size_t Offset, typename T>
void write_be(std::span<std::uint8_t, NTP_MESSAGE_LENGTH> message, T value) {
    auto field = message.template subspan<Offset, sizeof(T)>();
    for (std::size_t i = field.size(); i-- > 0;) {
        field[i] = static_cast<std::uint8_t>(value);
        value = static_cast<T>(value >> BITS_PER_BYTE);
    }
}

/// Convert a 32-bit NTP fraction-of-a-second to microseconds.
///
/// Exact integer arithmetic rather than a double multiply: the double form
/// links the whole soft-float runtime (__aeabi_ddiv and friends) into the
/// image for one operation. The result cannot exceed 999999, so no clamp is
/// needed -- (2^32 - 1) * 10^6 >> 32 == 999999.
std::uint32_t fraction_to_microseconds(std::uint32_t fraction) {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(fraction) * MICROSECONDS_PER_SECOND) >>
        FIELD_BITS);
}

bool in_plausibility_window(std::uint64_t epoch_seconds) {
    return epoch_seconds >= MIN_EPOCH_SECONDS &&
           epoch_seconds < MAX_EPOCH_SECONDS;
}

// What makes the era choice below decidable. The two readings of a seconds
// field differ by exactly one era, so a window narrower than an era cannot
// contain both of them: at most one candidate is ever accepted, and no tie
// needs breaking. A wider window would make the same field value mean two
// accepted instants, with nothing in the message to say which.
static_assert(EPOCH_WINDOW_SECONDS < NTP_ERA_SECONDS,
              "the plausibility window must be narrower than an NTP era");

/// Rebase an NTP seconds field onto the Unix epoch, or nullopt if no era puts
/// it inside the plausibility window.
///
/// NR-5, VR-6. The field counts seconds in the current era and carries no era
/// number, so one value denotes one instant per era and disambiguating it needs
/// an approximate time this code does not have. The plausibility window is that
/// approximate time: the era is whichever one lands inside it. A build whose
/// floor predates the 2036-02-07 rollover therefore accepts era 0 only, and the
/// same expression starts accepting era 1 once a release moves the floor past
/// it -- without the client ever having to guess.
std::optional<std::uint64_t> rebase_to_epoch(std::uint32_t seconds_since_1900) {
    const auto field = static_cast<std::uint64_t>(seconds_since_1900);

    // Era 0 exists as a candidate only above the delta; below it the
    // subtraction would be a pre-1970 time, which this library does not accept
    // under any era.
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
    // The nonce goes in the transmit timestamp, which the server echoes into
    // the reply's originate field. parse_ntp_response() requires that echo.
    write_be<TRANSMIT_OFFSET, std::uint64_t>(message, nonce);
    return message;
}

NtpParse parse_ntp_response(
    std::span<const std::uint8_t, NTP_MESSAGE_LENGTH> message,
    std::uint64_t expected_originate) {
    // VR-4, and first because it is the only check that establishes the
    // message is ours at all. The originate field echoes the nonce we sent;
    // without that echo a replayed capture of a genuine reply is
    // indistinguishable from a fresh one, which is the whole anti-replay story
    // in unauthenticated SNTP. Running it ahead of the content checks is what
    // makes ORIGINATE_MISMATCH the only verdict an unauthenticated datagram can
    // produce, so the caller can drop those without dropping a real server's
    // diagnosis of itself (VR-7).
    if (read_be<ORIGINATE_OFFSET, std::uint64_t>(message) !=
        expected_originate) {
        return {.reason = NtpReject::ORIGINATE_MISMATCH,
                .sample = std::nullopt};
    }

    const std::uint8_t flags = message[FLAGS_OFFSET];

    // Decoded through `unsigned` throughout: a std::uint8_t promotes to the
    // signed int before any shift or mask, which is signed bitwise arithmetic
    // on wire data (CXX-03).
    const auto flag_bits = static_cast<unsigned>(flags);

    // VR-1. Mode 4 is a server reply; anything else is not an answer to us.
    if ((flag_bits & MODE_MASK) != SERVER_MODE) {
        return {.reason = NtpReject::MODE, .sample = std::nullopt};
    }

    // VR-2. Leap indicator 3 is the alarm condition: the server's own clock is
    // not synchronized, so its timestamp is meaningless (RFC 4330 §4).
    if (((flag_bits >> LEAP_SHIFT) & LEAP_MASK) == LEAP_ALARM_VALUE) {
        return {.reason = NtpReject::LEAP_ALARM, .sample = std::nullopt};
    }

    // VR-3. Stratum 0 is Kiss-o'-Death and 16 means unsynchronized; both carry
    // a timestamp that must not be trusted (RFC 4330 §8, §5).
    const std::uint8_t stratum = message[STRATUM_OFFSET];
    if (stratum < MIN_USABLE_STRATUM || stratum > MAX_USABLE_STRATUM) {
        return {.reason = NtpReject::STRATUM, .sample = std::nullopt};
    }

    const auto seconds_since_1900 =
        read_be<TRANSMIT_OFFSET, std::uint32_t>(message);

    // VR-5. A zero transmit timestamp is "unset", which is what a server that
    // has itself never synchronized sends. Refused ahead of the rebase, which
    // has no way to tell an unset field from a deliberate one.
    if (seconds_since_1900 == 0) {
        return {.reason = NtpReject::TIMESTAMP_ZERO, .sample = std::nullopt};
    }

    // VR-6. The window is the whole bound on what a reply can claim: a field
    // value it does not admit under either era is refused here, whatever the
    // rest of the message looked like. It is also what keeps the multiply in
    // compute_epoch_offset() inside int64 (CXX-12.4).
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
