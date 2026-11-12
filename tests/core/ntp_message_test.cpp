// SPDX-License-Identifier: MIT
// Wire validation boundaries (VR-1..VR-6) and requests (NR-1, NR-2, NR-5,
// NR-6).

#include "ntp_message.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <pico_time/core/epoch.hpp>
#include <span>

namespace {

using pico_time::MAX_EPOCH_SECONDS;
using pico_time::MIN_EPOCH_SECONDS;
using pico_time::NTP_MESSAGE_LENGTH;
using pico_time::NtpParse;
using pico_time::NtpReject;

// Seconds between the NTP epoch (1900) and the Unix epoch (1970).
constexpr std::uint64_t NTP_DELTA = 2208988800ULL;

// One NTP era: the span of the 32-bit seconds field.
constexpr std::uint64_t NTP_ERA_SECONDS = 4294967296ULL;

// First instant of era 1, 2036-02-07T06:28:16Z, in Unix epoch seconds.
constexpr std::uint64_t FIRST_ERA1_EPOCH = NTP_ERA_SECONDS - NTP_DELTA;

constexpr std::uint64_t TEST_NONCE = 0x0123456789ABCDEFULL;

// 2026-01-01T00:00:00Z, comfortably inside the plausibility window and below
// the era-0 rollover.
constexpr std::uint64_t EPOCH_2026 = 1767225600ULL;
constexpr std::uint32_t ERA0_FIELD_2026 =
    static_cast<std::uint32_t>(EPOCH_2026 + NTP_DELTA);

/// A well-formed server reply, mutable field by field.
class Message {
   public:
    Message() {
        set_flags(LEAP_NONE, VERSION_4, MODE_SERVER);
        bytes_[STRATUM_OFFSET] = STRATUM_SECONDARY;
        set_u64(ORIGINATE_OFFSET, TEST_NONCE);
        set_u32(TRANSMIT_OFFSET, ERA0_FIELD_2026);
    }

    Message& leap(std::uint8_t value) {
        set_flags(value, VERSION_4, MODE_SERVER);
        return *this;
    }

    Message& mode(std::uint8_t value) {
        set_flags(LEAP_NONE, VERSION_4, value);
        return *this;
    }

    Message& stratum(std::uint8_t value) {
        bytes_[STRATUM_OFFSET] = value;
        return *this;
    }

    Message& originate(std::uint64_t value) {
        set_u64(ORIGINATE_OFFSET, value);
        return *this;
    }

    Message& transmit_seconds(std::uint32_t value) {
        set_u32(TRANSMIT_OFFSET, value);
        return *this;
    }

    Message& transmit_fraction(std::uint32_t value) {
        set_u32(TRANSMIT_OFFSET + 4, value);
        return *this;
    }

    [[nodiscard]] NtpParse parse(std::uint64_t nonce = TEST_NONCE) const {
        return pico_time::parse_ntp_response(
            std::span<const std::uint8_t, NTP_MESSAGE_LENGTH>(bytes_), nonce);
    }

    static constexpr std::uint8_t LEAP_NONE = 0;
    static constexpr std::uint8_t LEAP_ALARM = 3;
    static constexpr std::uint8_t MODE_CLIENT = 3;
    static constexpr std::uint8_t MODE_SERVER = 4;
    static constexpr std::uint8_t STRATUM_SECONDARY = 2;

   private:
    static constexpr std::size_t FLAGS_OFFSET = 0;
    static constexpr std::size_t STRATUM_OFFSET = 1;
    static constexpr std::size_t ORIGINATE_OFFSET = 24;
    static constexpr std::size_t TRANSMIT_OFFSET = 40;
    static constexpr std::uint8_t VERSION_4 = 4;
    static constexpr unsigned LEAP_SHIFT = 6;
    static constexpr unsigned VERSION_SHIFT = 3;

    void set_flags(std::uint8_t leap_value, std::uint8_t version,
                   std::uint8_t mode_value) {
        // Composed through `unsigned`: a std::uint8_t promotes to signed int
        // before the shift, which is signed bitwise arithmetic.
        const unsigned bits =
            (static_cast<unsigned>(leap_value) << LEAP_SHIFT) |
            (static_cast<unsigned>(version) << VERSION_SHIFT) |
            static_cast<unsigned>(mode_value);
        bytes_[FLAGS_OFFSET] = static_cast<std::uint8_t>(bits);
    }

    void set_u32(std::size_t offset, std::uint32_t value) {
        set_u64(offset, value, sizeof(std::uint32_t));
    }

    void set_u64(std::size_t offset, std::uint64_t value,
                 std::size_t width = sizeof(std::uint64_t)) {
        for (std::size_t i = width; i-- > 0;) {
            bytes_[offset + i] = static_cast<std::uint8_t>(value);
            value >>= 8U;
        }
    }

    std::array<std::uint8_t, NTP_MESSAGE_LENGTH> bytes_{};
};

TEST(NtpMessage, AcceptsAWellFormedServerReply) {
    const NtpParse parse = Message().parse();

    EXPECT_EQ(parse.reason, NtpReject::NONE);
    if (!parse.sample.has_value()) {
        FAIL() << "a well-formed reply was rejected";
        return;
    }
    EXPECT_EQ(parse.sample->epoch_seconds, EPOCH_2026);
    EXPECT_EQ(parse.sample->epoch_microseconds, 0U);
}

// Mode validation (VR-1).

TEST(NtpMessage, RejectsAnythingButAServerReply) {
    EXPECT_EQ(Message().mode(Message::MODE_CLIENT).parse().reason,
              NtpReject::MODE);
    EXPECT_EQ(Message().mode(5).parse().reason, NtpReject::MODE);
    EXPECT_EQ(Message().mode(0).parse().reason, NtpReject::MODE);
}

// Leap indicator validation (VR-2).

TEST(NtpMessage, RejectsTheLeapAlarmCondition) {
    // LI 3 denotes an unsynchronised server clock (RFC 5905 §7.3).
    EXPECT_EQ(Message().leap(Message::LEAP_ALARM).parse().reason,
              NtpReject::LEAP_ALARM);
}

TEST(NtpMessage, AcceptsThePendingLeapSecondIndicators) {
    for (std::uint8_t leap = 0; leap < Message::LEAP_ALARM; ++leap) {
        EXPECT_EQ(Message().leap(leap).parse().reason, NtpReject::NONE)
            << "leap indicator " << static_cast<int>(leap);
    }
}

// Stratum validation (VR-3).

TEST(NtpMessage, RejectsUnusableStrata) {
    EXPECT_EQ(Message().stratum(0).parse().reason, NtpReject::STRATUM)
        << "stratum 0 is Kiss-o'-Death";
    EXPECT_EQ(Message().stratum(16).parse().reason, NtpReject::STRATUM)
        << "stratum 16 means unsynchronized";
    EXPECT_EQ(Message().stratum(17).parse().reason, NtpReject::STRATUM)
        << "stratum 17+ is reserved";
    EXPECT_EQ(Message().stratum(255).parse().reason, NtpReject::STRATUM);
}

TEST(NtpMessage, AcceptsPrimaryAndSecondaryStrata) {
    EXPECT_EQ(Message().stratum(1).parse().reason, NtpReject::NONE);
    EXPECT_EQ(Message().stratum(15).parse().reason, NtpReject::NONE);
}

// Anti-replay nonce validation (VR-4).

TEST(NtpMessage, RejectsAReplyToADifferentRequest) {
    EXPECT_EQ(Message().originate(TEST_NONCE + 1).parse().reason,
              NtpReject::ORIGINATE_MISMATCH);
    EXPECT_EQ(Message().originate(0).parse().reason,
              NtpReject::ORIGINATE_MISMATCH);
}

TEST(NtpMessage, RejectsAReplayedCapture) {
    // A captured reply has valid fields but the previous request's nonce.
    const Message captured;
    constexpr std::uint64_t LATER_REQUEST_NONCE = 0xFEDCBA9876543210ULL;

    EXPECT_EQ(captured.parse().reason, NtpReject::NONE);
    EXPECT_EQ(captured.parse(LATER_REQUEST_NONCE).reason,
              NtpReject::ORIGINATE_MISMATCH);
}

TEST(NtpMessage, AnUnauthenticatedDatagramIsOnlyEverAnOriginateMismatch) {
    // A nonce mismatch takes precedence over malformed content so the caller
    // can drop the datagram without ending the attempt (VR-4, VR-7).
    constexpr std::uint64_t NOT_OURS = TEST_NONCE + 1;

    EXPECT_EQ(Message().originate(NOT_OURS).mode(0).parse().reason,
              NtpReject::ORIGINATE_MISMATCH);
    EXPECT_EQ(
        Message().originate(NOT_OURS).leap(Message::LEAP_ALARM).parse().reason,
        NtpReject::ORIGINATE_MISMATCH);
    EXPECT_EQ(Message().originate(NOT_OURS).stratum(0).parse().reason,
              NtpReject::ORIGINATE_MISMATCH);
    EXPECT_EQ(Message().originate(NOT_OURS).transmit_seconds(0).parse().reason,
              NtpReject::ORIGINATE_MISMATCH);
    EXPECT_EQ(Message().originate(NOT_OURS).transmit_seconds(1).parse().reason,
              NtpReject::ORIGINATE_MISMATCH);
}

// Zero timestamp rejection (VR-5).

TEST(NtpMessage, RejectsAnUnsetTransmitTimestamp) {
    // Zero is rejected before era rebasing can place it in 2036.
    const NtpParse parse = Message().transmit_seconds(0).parse();

    EXPECT_EQ(parse.reason, NtpReject::TIMESTAMP_ZERO);
    EXPECT_FALSE(parse.sample.has_value());
}

TEST(NtpMessage, RejectsAnUnsetTimestampEvenWithANonZeroFraction) {
    EXPECT_EQ(Message()
                  .transmit_seconds(0)
                  .transmit_fraction(0xFFFFFFFFU)
                  .parse()
                  .reason,
              NtpReject::TIMESTAMP_ZERO);
}

// Plausibility window and 2036 rollover cases (VR-6).

TEST(NtpMessage, RejectsATimestampBelowThePlausibilityFloor) {
    // Both timestamps predate the floor but can be rebased in era 0.
    const auto just_below =
        static_cast<std::uint32_t>(MIN_EPOCH_SECONDS + NTP_DELTA - 1);
    EXPECT_EQ(Message().transmit_seconds(just_below).parse().reason,
              NtpReject::TIMESTAMP_IMPLAUSIBLE);

    constexpr std::uint32_t FIELD_1995 = 3000000000U;
    EXPECT_EQ(Message().transmit_seconds(FIELD_1995).parse().reason,
              NtpReject::TIMESTAMP_IMPLAUSIBLE);
}

TEST(NtpMessage, AcceptsATimestampExactlyAtThePlausibilityFloor) {
    const auto at_floor =
        static_cast<std::uint32_t>(MIN_EPOCH_SECONDS + NTP_DELTA);
    const NtpParse parse = Message().transmit_seconds(at_floor).parse();

    ASSERT_EQ(parse.reason, NtpReject::NONE);
    if (!parse.sample.has_value()) {
        FAIL() << "the floor itself must be accepted";
        return;
    }
    EXPECT_EQ(parse.sample->epoch_seconds, MIN_EPOCH_SECONDS);
}

TEST(NtpMessage, AcceptsTheSecondBelowThePlausibilityCeiling) {
    const auto below_ceiling =
        static_cast<std::uint32_t>(MAX_EPOCH_SECONDS + NTP_DELTA - 1);
    const NtpParse parse = Message().transmit_seconds(below_ceiling).parse();

    ASSERT_EQ(parse.reason, NtpReject::NONE);
    if (!parse.sample.has_value()) {
        FAIL() << "the last second inside the window must be accepted";
        return;
    }
    EXPECT_EQ(parse.sample->epoch_seconds, MAX_EPOCH_SECONDS - 1);
}

TEST(NtpMessage, RejectsATimestampAtAndAboveThePlausibilityCeiling) {
    // The exclusive ceiling is reachable by an era-0 timestamp.
    const auto at_ceiling =
        static_cast<std::uint32_t>(MAX_EPOCH_SECONDS + NTP_DELTA);
    EXPECT_EQ(Message().transmit_seconds(at_ceiling).parse().reason,
              NtpReject::TIMESTAMP_IMPLAUSIBLE);

    EXPECT_EQ(Message().transmit_seconds(0xFFFFFFFFU).parse().reason,
              NtpReject::TIMESTAMP_IMPLAUSIBLE)
        << "the last second of era 0 is 2036-02-07, past the ceiling";
}

// The configured window excludes all era-1 timestamps (NR-5).
TEST(NtpMessage, RefusesEveryEra1FieldWhileTheWindowPredatesTheRollover) {
    static_assert(MAX_EPOCH_SECONDS < FIRST_ERA1_EPOCH,
                  "this test states what the window's position implies; a "
                  "window reaching past the rollover needs the opposite case");

    // Field 1 represents 2036-02-07T06:28:17Z in era 1 and predates 1970 in era
    // 0.
    EXPECT_EQ(Message().transmit_seconds(1).parse().reason,
              NtpReject::TIMESTAMP_IMPLAUSIBLE);

    // The two ends of the era-1 field range, and the era boundary itself.
    const auto last_era1 = static_cast<std::uint32_t>(NTP_DELTA - 1);
    EXPECT_EQ(Message().transmit_seconds(last_era1).parse().reason,
              NtpReject::TIMESTAMP_IMPLAUSIBLE);

    const auto unix_epoch = static_cast<std::uint32_t>(NTP_DELTA);
    EXPECT_EQ(Message().transmit_seconds(unix_epoch).parse().reason,
              NtpReject::TIMESTAMP_IMPLAUSIBLE)
        << "the first era-0 field is 1970, below the floor";
}

// A window narrower than an era admits at most one candidate (FR-8, NR-5).
TEST(NtpMessage, ThePlausibilityWindowIsNarrowerThanAnNtpEra) {
    EXPECT_LT(pico_time::EPOCH_WINDOW_SECONDS, NTP_ERA_SECONDS);
    EXPECT_EQ(MAX_EPOCH_SECONDS - MIN_EPOCH_SECONDS,
              pico_time::EPOCH_WINDOW_SECONDS);
}

// Fraction conversion (NR-6).
TEST(NtpMessage, ConvertsTheFractionExactly) {
    // The largest fraction must produce 999999 microseconds without clamping.
    struct Case {
        std::uint32_t fraction;
        std::uint32_t microseconds;
    };

    constexpr std::array<Case, 5> CASES{{
        {.fraction = 0x00000000U, .microseconds = 0U},
        {.fraction = 0x80000000U, .microseconds = 500000U},
        {.fraction = 0x40000000U, .microseconds = 250000U},
        {.fraction = 0xC0000000U, .microseconds = 750000U},
        {.fraction = 0xFFFFFFFFU, .microseconds = 999999U},
    }};

    for (const Case& test_case : CASES) {
        const NtpParse parse =
            Message().transmit_fraction(test_case.fraction).parse();
        if (!parse.sample.has_value()) {
            FAIL() << "fraction " << test_case.fraction << " was rejected";
            return;
        }
        EXPECT_EQ(parse.sample->epoch_microseconds, test_case.microseconds)
            << "fraction " << test_case.fraction;
    }
}

// The request carries client flags and a nonce (NR-1, NR-2).
TEST(NtpMessage, BuildsAClientRequestCarryingTheNonce) {
    constexpr std::uint8_t CLIENT_FLAGS = 0x1BU;  // LI 0, VN 3, Mode 3
    const auto request = pico_time::build_ntp_request(TEST_NONCE);

    EXPECT_EQ(request[0], CLIENT_FLAGS);

    // The nonce must occupy transmit, which the server echoes as originate.
    std::uint64_t transmitted = 0;
    for (std::size_t i = 40; i < 48; ++i) {
        transmitted = (transmitted << 8U) | request[i];
    }
    EXPECT_EQ(transmitted, TEST_NONCE);
}

TEST(NtpMessage, RequestAndResponseAgreeOnWhereTheNonceLives) {
    // The parser must accept the request nonce echoed in the originate field.
    // A round trip exposes mismatched field offsets.
    constexpr std::uint64_t NONCE = 0xDEADBEEFCAFEF00DULL;
    const auto request = pico_time::build_ntp_request(NONCE);

    std::uint64_t echoed = 0;
    for (std::size_t i = 40; i < 48; ++i) {
        echoed = (echoed << 8U) | request[i];
    }

    EXPECT_EQ(Message().originate(echoed).parse(NONCE).reason, NtpReject::NONE);
}

TEST(NtpMessage, EveryRejectionHasADescription) {
    constexpr std::array<NtpReject, 7> REASONS{{
        NtpReject::NONE,
        NtpReject::ORIGINATE_MISMATCH,
        NtpReject::MODE,
        NtpReject::LEAP_ALARM,
        NtpReject::STRATUM,
        NtpReject::TIMESTAMP_ZERO,
        NtpReject::TIMESTAMP_IMPLAUSIBLE,
    }};

    for (const NtpReject reason : REASONS) {
        const char* text = pico_time::describe(reason);
        ASSERT_NE(text, nullptr);
        EXPECT_STRNE(text, "unknown");
    }
}

}  // namespace
