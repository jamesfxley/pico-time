// SPDX-License-Identifier: MIT
// The validation rules of VR-1..VR-6, each at its boundary, and the request
// side of NR-1, NR-2, NR-5 and NR-6.
//
// Every case here is one an attacker or a broken server can put on the wire,
// and none of them is reachable on device without standing up a hostile NTP
// server -- which is the reason parse_ntp_response() is a pure function over
// 48 bytes rather than something welded to a pbuf.

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

// Seconds between the NTP epoch (1900) and the Unix epoch (1970). Named
// because every field value below is derived from it.
constexpr std::uint64_t NTP_DELTA = 2208988800ULL;

// One NTP era: the span of the 32-bit seconds field.
constexpr std::uint64_t NTP_ERA_SECONDS = 4294967296ULL;

// The first instant of era 1, 2036-02-07T06:28:16Z, as a Unix epoch second.
// Where the window sits relative to this is what decides which eras a build
// can accept at all.
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

// --- the happy path, so every rejection below is attributable ---------------

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

// --- VR-1: mode ------------------------------------------------------------

TEST(NtpMessage, RejectsAnythingButAServerReply) {
    EXPECT_EQ(Message().mode(Message::MODE_CLIENT).parse().reason,
              NtpReject::MODE);
    EXPECT_EQ(Message().mode(5).parse().reason, NtpReject::MODE);
    EXPECT_EQ(Message().mode(0).parse().reason, NtpReject::MODE);
}

// --- VR-2: leap indicator --------------------------------------------------

TEST(NtpMessage, RejectsTheLeapAlarmCondition) {
    // LI 3 means the server's own clock is not synchronized. Before this check
    // existed the two leap bits were masked away entirely, so a server
    // advertising an unusable clock set this board's.
    EXPECT_EQ(Message().leap(Message::LEAP_ALARM).parse().reason,
              NtpReject::LEAP_ALARM);
}

TEST(NtpMessage, AcceptsThePendingLeapSecondIndicators) {
    for (std::uint8_t leap = 0; leap < Message::LEAP_ALARM; ++leap) {
        EXPECT_EQ(Message().leap(leap).parse().reason, NtpReject::NONE)
            << "leap indicator " << static_cast<int>(leap);
    }
}

// --- VR-3: stratum ---------------------------------------------------------

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

// --- VR-4: the anti-replay nonce -------------------------------------------

TEST(NtpMessage, RejectsAReplyToADifferentRequest) {
    EXPECT_EQ(Message().originate(TEST_NONCE + 1).parse().reason,
              NtpReject::ORIGINATE_MISMATCH);
    EXPECT_EQ(Message().originate(0).parse().reason,
              NtpReject::ORIGINATE_MISMATCH);
}

TEST(NtpMessage, RejectsAReplayedCapture) {
    // A genuine reply, captured and replayed against a later request. Every
    // other field is valid -- the nonce is the only thing that distinguishes
    // it, which is the whole anti-replay story in unauthenticated SNTP.
    const Message captured;
    constexpr std::uint64_t LATER_REQUEST_NONCE = 0xFEDCBA9876543210ULL;

    EXPECT_EQ(captured.parse().reason, NtpReject::NONE);
    EXPECT_EQ(captured.parse(LATER_REQUEST_NONCE).reason,
              NtpReject::ORIGINATE_MISMATCH);
}

TEST(NtpMessage, AnUnauthenticatedDatagramIsOnlyEverAnOriginateMismatch) {
    // VR-4 runs ahead of every content check, so a datagram that has not shown
    // it belongs to this request cannot be reported as anything else however
    // else it is malformed. The caller relies on exactly that to drop it
    // without ending the attempt (VR-7); any other verdict reachable from an
    // unauthenticated datagram is a denial-of-synchronization channel.
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

// --- VR-5: the zero timestamp ----------------------------------------------

TEST(NtpMessage, RejectsAnUnsetTransmitTimestamp) {
    // A stratum-1 server that has never synchronized sends zero here. Zero is
    // refused for being unset rather than left to the window, because a window
    // that reaches era 1 would read it as a real instant just past the
    // rollover -- and refusing it here holds whatever the window is.
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

// --- VR-6: the plausibility window and the 2036 rollover -------------------

TEST(NtpMessage, RejectsATimestampBelowThePlausibilityFloor) {
    // One second below the floor, and a 1995 timestamp: both are era-0 values
    // that rebase cleanly and are simply too old to be a time this firmware
    // could legitimately be told.
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
    // The half-open end of the window, which a message can only reach because
    // the ceiling sits inside the range the rebase produces. A ceiling above
    // that range is one no reply can ever be refused by.
    const auto at_ceiling =
        static_cast<std::uint32_t>(MAX_EPOCH_SECONDS + NTP_DELTA);
    EXPECT_EQ(Message().transmit_seconds(at_ceiling).parse().reason,
              NtpReject::TIMESTAMP_IMPLAUSIBLE);

    EXPECT_EQ(Message().transmit_seconds(0xFFFFFFFFU).parse().reason,
              NtpReject::TIMESTAMP_IMPLAUSIBLE)
        << "the last second of era 0 is 2036-02-07, past the ceiling";
}

// NR-5. The era is chosen by the window, so which era a build accepts follows
// from where its window sits. This one predates the 2036-02-07 rollover, so
// era 1 is out of reach: no field value can be read into it and be accepted.
TEST(NtpMessage, RefusesEveryEra1FieldWhileTheWindowPredatesTheRollover) {
    static_assert(MAX_EPOCH_SECONDS < FIRST_ERA1_EPOCH,
                  "this test states what the window's position implies; a "
                  "window reaching past the rollover needs the opposite case");

    // Field 1 reads as 2036-02-07T06:28:17Z under era 1 and as nothing at all
    // under era 0. A window that accepts it is a window one forged datagram
    // steps the clock a decade across.
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

// FR-8, NR-5. The invariant the era choice rests on: two readings of a field
// differ by exactly one era, so a window narrower than an era can hold at most
// one of them. Widening the window past this makes a field value mean two
// accepted instants with nothing in the message to separate them.
TEST(NtpMessage, ThePlausibilityWindowIsNarrowerThanAnNtpEra) {
    EXPECT_LT(pico_time::EPOCH_WINDOW_SECONDS, NTP_ERA_SECONDS);
    EXPECT_EQ(MAX_EPOCH_SECONDS - MIN_EPOCH_SECONDS,
              pico_time::EPOCH_WINDOW_SECONDS);
}

// --- The fraction conversion ------------------------------------------------

// NR-6.
TEST(NtpMessage, ConvertsTheFractionExactly) {
    // Integer arithmetic, not a double multiply: the double form linked the
    // whole soft-float runtime into the firmware image. The top of the range
    // must land on 999999 without a clamp.
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

// --- The request, and the round trip that ties it to the check -------------

// NR-1 (the request's own shape) and NR-2 (the nonce it carries).
TEST(NtpMessage, BuildsAClientRequestCarryingTheNonce) {
    constexpr std::uint8_t CLIENT_FLAGS = 0x1BU;  // LI 0, VN 3, Mode 3
    const auto request = pico_time::build_ntp_request(TEST_NONCE);

    EXPECT_EQ(request[0], CLIENT_FLAGS);

    // The nonce must land in the transmit field, because that is the field the
    // server echoes into originate and parse_ntp_response() compares.
    std::uint64_t transmitted = 0;
    for (std::size_t i = 40; i < 48; ++i) {
        transmitted = (transmitted << 8U) | request[i];
    }
    EXPECT_EQ(transmitted, TEST_NONCE);
}

TEST(NtpMessage, RequestAndResponseAgreeOnWhereTheNonceLives) {
    // The end-to-end property: whatever build_ntp_request() puts on the wire
    // is what a compliant server echoes back, and parse_ntp_response() must
    // accept exactly that. A round trip catches an offset mismatch between the
    // two that neither side's own test would.
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
