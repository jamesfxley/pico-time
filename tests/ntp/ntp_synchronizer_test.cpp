// SPDX-License-Identifier: MIT
// Scripted retry, timeout and slot-release cases (NR-3, AR-2, AR-6).
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <pico_time/ntp/ntp_error.hpp>
#include <pico_time/ntp/ntp_synchronizer.hpp>
#include <pico_time/ntp/ntp_transport.hpp>
#include <pico_time/testing/fake_ntp_transport.hpp>
#include <vector>

namespace pico_time {
namespace {

using pico_time::testing::FakeNtpTransport;

constexpr auto SHORT_TIMEOUT = std::chrono::milliseconds(200);
constexpr auto GENEROUS_TIMEOUT = std::chrono::milliseconds(1000);
constexpr auto NO_TIMEOUT = std::chrono::milliseconds(0);

// Where a 32-bit millisecond counter returns to zero.
constexpr std::uint64_t CLOCK_WRAP_32 = 0x100000000U;

// Independent wire constants keep input construction separate from parsing.
constexpr std::size_t MESSAGE_LENGTH = 48;
constexpr std::size_t ORIGINATE_OFFSET = 24;
constexpr std::size_t TRANSMIT_OFFSET = 40;
constexpr std::uint32_t NTP_ERA_OFFSET = 2208988800U;
constexpr std::uint64_t EPOCH_SECONDS = 1767225600U;  // 2026-01-01Z

/// A reply to nonce with a checkable time inside the window (VR-6).
std::vector<std::uint8_t> good_reply(std::uint64_t nonce) {
    std::vector<std::uint8_t> reply(MESSAGE_LENGTH, 0);
    reply[0] = 0x24;  // leap 0, version 4, mode 4 (server)
    reply[1] = 2;     // stratum
    for (std::size_t i = 0; i < 8; ++i) {
        reply[ORIGINATE_OFFSET + i] =
            static_cast<std::uint8_t>(nonce >> (56 - (8 * i)));
    }
    const auto seconds =
        static_cast<std::uint32_t>(EPOCH_SECONDS) + NTP_ERA_OFFSET;
    for (std::size_t i = 0; i < 4; ++i) {
        reply[TRANSMIT_OFFSET + i] =
            static_cast<std::uint8_t>(seconds >> (24 - (8 * i)));
    }
    return reply;
}

/// Script a cached lookup and a reply on the first poll.
void arrange_answering(FakeNtpTransport& transport) {
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    transport.arrive_on_poll(1, FakeNtpTransport::address(1),
                             good_reply(transport.next_nonce()));
}

TEST(NtpSynchronizer, AcceptsAReplyAndReportsTheTime) {
    FakeNtpTransport transport;
    arrange_answering(transport);
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);

    ASSERT_TRUE(result.ok()) << describe(result.error());
    EXPECT_EQ(result.value().epoch_seconds, EPOCH_SECONDS);
    EXPECT_EQ(transport.open_calls(), 1U);
    EXPECT_EQ(transport.send_calls(), 1U);
    // These assertions check closure, not its order relative to reading sample.
    // The fake cannot deliver a datagram outside poll().
    EXPECT_EQ(transport.close_calls(), 1U);
    EXPECT_FALSE(transport.is_open());
    EXPECT_EQ(sync.slots_in_use(), 0U);
}

TEST(NtpSynchronizer, RefusesANonPositiveTimeoutWithoutTouchingTheNetwork) {
    FakeNtpTransport transport;
    arrange_answering(transport);
    NtpSynchronizer sync(transport, "pool.ntp.org");

    for (const std::chrono::milliseconds timeout :
         {NO_TIMEOUT, std::chrono::milliseconds{-1}}) {
        const auto result = sync.synchronize(timeout);

        ASSERT_FALSE(result.ok());
        EXPECT_EQ(result.error(), NtpError::INVALID_TIMEOUT);
    }
    EXPECT_EQ(transport.resolve_calls(), 0U);
    EXPECT_EQ(transport.open_calls(), 0U);
}

TEST(NtpSynchronizer, TriesEveryAttemptBeforeGivingUp) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::TIMED_OUT);
    EXPECT_EQ(transport.open_calls(), MAX_REQUEST_ATTEMPTS);
    EXPECT_EQ(transport.send_calls(), MAX_REQUEST_ATTEMPTS);
    EXPECT_EQ(transport.close_calls(), MAX_REQUEST_ATTEMPTS);
    EXPECT_EQ(sync.slots_in_use(), 0U);
}

// A narrowing test clock exercises wrap during an attempt.
// The production adapter supplies 64-bit milliseconds.
TEST(NtpSynchronizer, AnAttemptSpanningTheClockWrapStillTimesOut) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    transport.set_narrow_clock(true);
    transport.set_now(CLOCK_WRAP_32 - (GENEROUS_TIMEOUT.count() / 2));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::TIMED_OUT);
    EXPECT_EQ(transport.open_calls(), MAX_REQUEST_ATTEMPTS);
}

TEST(NtpSynchronizer, WaitsBetweenAttemptsButNotAfterTheLast) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);
    ASSERT_FALSE(result.ok());

    std::size_t retry_waits = 0;
    for (const auto wait : transport.waits()) {
        if (wait == RETRY_DELAY) {
            ++retry_waits;
        }
    }
    EXPECT_EQ(retry_waits, MAX_REQUEST_ATTEMPTS - 1);
}

TEST(NtpSynchronizer, GivesEachAttemptItsOwnNonce) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);
    ASSERT_FALSE(result.ok());

    // A reused nonce would make a late reply to an abandoned attempt
    // indistinguishable from a fresh one (VR-4).
    EXPECT_EQ(transport.next_nonce(), 1U + MAX_REQUEST_ATTEMPTS);
}

TEST(NtpSynchronizer, AResolutionFailureEndsTheAttempt) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::FAILED);
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::DNS_FAILED);
    EXPECT_EQ(transport.send_calls(), 0U);
    // Every attempt still closed the endpoint it opened.
    EXPECT_EQ(transport.close_calls(), transport.open_calls());
}

TEST(NtpSynchronizer, AnEndpointThatWillNotOpenIsNotADnsFailure) {
    FakeNtpTransport transport;
    transport.set_open_result(false);
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::SOCKET_FAILED);
    EXPECT_EQ(transport.resolve_calls(), 0U);
    // Socket creation failure is retryable.
    EXPECT_TRUE(retryable(result.error()));
}

TEST(NtpSynchronizer, ASendFailureEndsTheAttempt) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    transport.set_send_result(false);
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::SEND_FAILED);
    // Send failure is retryable.
    EXPECT_TRUE(retryable(result.error()));
}

// A rejection ends the call at once, so its reason is reported, not a timeout.
TEST(NtpSynchronizer, ARefusedReplyReportsWhyRatherThanTimingOut) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    // Each reply echoes its request nonce but reports client mode (VR-1).
    for (std::size_t attempt = 1; attempt <= MAX_REQUEST_ATTEMPTS; ++attempt) {
        std::vector<std::uint8_t> reply = good_reply(attempt);
        reply[0] = 0x23;  // mode 3
        transport.arrive_on_poll(attempt, FakeNtpTransport::address(1), reply);
    }
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::REJECTED_MODE);
    // Reply rejection is classified as non-retryable.
    EXPECT_FALSE(retryable(result.error()));
    EXPECT_EQ(transport.send_calls(), 1U);
}

// NR-3: a rejection ends the call, so the server is not asked again.
TEST(NtpSynchronizer, ARejectionEndsTheCallWithoutASecondAttempt) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    std::vector<std::uint8_t> refused = good_reply(1);
    refused[1] = 0;  // stratum 0: Kiss-o'-Death (VR-3)
    transport.arrive_on_poll(1, FakeNtpTransport::address(1), refused);
    transport.arrive_on_poll(2, FakeNtpTransport::address(1), good_reply(2));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::REJECTED_STRATUM);
    EXPECT_EQ(transport.open_calls(), 1U);
    EXPECT_EQ(transport.send_calls(), 1U);
    for (const auto wait : transport.waits()) {
        EXPECT_NE(wait, RETRY_DELAY);
    }
    EXPECT_EQ(sync.slots_in_use(), 0U);
}

// NR-3: a rejection on a later attempt also ends the call.
TEST(NtpSynchronizer, ARejectionAfterATimeoutEndsTheCall) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    // Attempt 2's refusal arrives on every poll: attempt 1 drops each copy
    // for its nonce and times out; attempt 2 takes the first one after its
    // send.
    std::vector<std::uint8_t> refused = good_reply(2);
    refused[1] = 0;  // stratum 0: Kiss-o'-Death (VR-3)
    const std::size_t polls = (2 * (SHORT_TIMEOUT / POLL_DELAY)) + 2;
    for (std::size_t poll = 1; poll <= polls; ++poll) {
        transport.arrive_on_poll(poll, FakeNtpTransport::address(1), refused);
    }
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::REJECTED_STRATUM);
    EXPECT_EQ(transport.open_calls(), 2U);
    EXPECT_EQ(transport.send_calls(), 2U);
    std::size_t retry_waits = 0;
    for (const auto wait : transport.waits()) {
        if (wait == RETRY_DELAY) {
            ++retry_waits;
        }
    }
    EXPECT_EQ(retry_waits, 1U);
    EXPECT_EQ(sync.slots_in_use(), 0U);
}

// NR-3: each reachable REJECTED_* code ends the call after one send.
TEST(NtpSynchronizer, EveryRejectedReplyEndsTheCallAfterOneSend) {
    // Leap indicator 3, version 4, mode 4 (server).
    constexpr std::uint8_t LEAP_ALARM_FLAGS = 0xE4;
    // One second past the NTP epoch: no era places it in the window (VR-6).
    constexpr std::uint32_t IMPLAUSIBLE_SECONDS = 1U;

    std::vector<std::uint8_t> mode = good_reply(1);
    mode[0] = 0x23;  // mode 3
    std::vector<std::uint8_t> leap_alarm = good_reply(1);
    leap_alarm[0] = LEAP_ALARM_FLAGS;
    std::vector<std::uint8_t> stratum = good_reply(1);
    stratum[1] = 0;  // Kiss-o'-Death (VR-3)
    std::vector<std::uint8_t> zero_timestamp = good_reply(1);
    for (std::size_t i = 0; i < 4; ++i) {
        zero_timestamp[TRANSMIT_OFFSET + i] = 0;
    }
    std::vector<std::uint8_t> implausible = good_reply(1);
    for (std::size_t i = 0; i < 4; ++i) {
        implausible[TRANSMIT_OFFSET + i] =
            static_cast<std::uint8_t>(IMPLAUSIBLE_SECONDS >> (24 - (8 * i)));
    }

    struct Case {
        NtpError error;
        std::vector<std::uint8_t> reply;
    };

    const std::array<Case, 5> cases = {{
        {.error = NtpError::REJECTED_MODE, .reply = mode},
        {.error = NtpError::REJECTED_LEAP_ALARM, .reply = leap_alarm},
        {.error = NtpError::REJECTED_STRATUM, .reply = stratum},
        {.error = NtpError::REJECTED_TIMESTAMP_ZERO, .reply = zero_timestamp},
        {.error = NtpError::REJECTED_TIMESTAMP_IMPLAUSIBLE,
         .reply = implausible},
    }};
    for (const Case& test_case : cases) {
        SCOPED_TRACE(describe(test_case.error));
        FakeNtpTransport transport;
        transport.set_resolve_result(ResolveStatus::RESOLVED);
        transport.arrive_on_poll(1, FakeNtpTransport::address(1),
                                 test_case.reply);
        NtpSynchronizer sync(transport, "pool.ntp.org");

        const auto result = sync.synchronize(GENEROUS_TIMEOUT);

        ASSERT_FALSE(result.ok());
        EXPECT_EQ(result.error(), test_case.error);
        EXPECT_EQ(transport.send_calls(), 1U);
        EXPECT_EQ(sync.slots_in_use(), 0U);
    }
}

TEST(NtpSynchronizer, ADatagramFromElsewhereDoesNotEndTheAttempt) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    // A well-formed reply from an address that is not the server's, then the
    // real one. The first must be dropped rather than accepted or counted.
    transport.arrive_on_poll(1, FakeNtpTransport::address(9),
                             good_reply(transport.next_nonce()));
    transport.arrive_on_poll(2, FakeNtpTransport::address(1),
                             good_reply(transport.next_nonce()));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);

    ASSERT_TRUE(result.ok()) << describe(result.error());
    EXPECT_EQ(transport.open_calls(), 1U);
}

TEST(NtpSynchronizer, ADatagramTooShortForAMessageDoesNotEndTheAttempt) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    // A short datagram leaves the attempt waiting for the complete reply
    // (VR-7).
    std::vector<std::uint8_t> truncated = good_reply(transport.next_nonce());
    truncated.pop_back();
    transport.arrive_on_poll(1, FakeNtpTransport::address(1), truncated);
    transport.arrive_on_poll(2, FakeNtpTransport::address(1),
                             good_reply(transport.next_nonce()));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);

    ASSERT_TRUE(result.ok()) << describe(result.error());
    EXPECT_GE(transport.poll_calls(), 2U);
    EXPECT_EQ(transport.open_calls(), 1U);
}

TEST(NtpSynchronizer, AReplyToAnotherRequestIsDroppedNotCounted) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    // A replay must not end an attempt before its valid reply arrives (VR-7).
    transport.arrive_on_poll(1, FakeNtpTransport::address(1), good_reply(999));
    transport.arrive_on_poll(2, FakeNtpTransport::address(1),
                             good_reply(transport.next_nonce()));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);

    ASSERT_TRUE(result.ok()) << describe(result.error());
    EXPECT_EQ(transport.send_calls(), 1U);
}

// The slot pool. These are the cases the seam exists for.

TEST(NtpSynchronizer, ALookupThatNeverLandsHoldsItsSlot) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::IN_PROGRESS);
    transport.resolve_on_poll(0, FakeNtpTransport::address(1));  // never
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::TIMED_OUT);
    // All attempts timed out during resolution, leaving their slots held
    // (AR-6).
    EXPECT_EQ(sync.slots_in_use(), MAX_REQUEST_ATTEMPTS);
    EXPECT_TRUE(transport.resolution_outstanding());
}

TEST(NtpSynchronizer, SlotExhaustionIsReportedAndIsRetryable) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::IN_PROGRESS);
    transport.resolve_on_poll(0, FakeNtpTransport::address(1));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    ASSERT_FALSE(sync.synchronize(SHORT_TIMEOUT).ok());
    ASSERT_EQ(sync.slots_in_use(), MAX_REQUEST_ATTEMPTS);

    // The next call must report slot exhaustion and must not spend three
    // attempts (AR-6).
    const std::size_t opens = transport.open_calls();
    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::NO_ATTEMPT_SLOT);
    EXPECT_EQ(transport.open_calls(), opens);
    // Slot exhaustion clears on its own.
    EXPECT_TRUE(retryable(result.error()));
}

// Slot exhaustion after network activity returns the preceding error (AR-6).
TEST(NtpSynchronizer, APoolEmptiedMidCallReportsTheAttemptNotThePool) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::IN_PROGRESS);
    transport.resolve_on_poll(0, FakeNtpTransport::address(1));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    ASSERT_FALSE(sync.synchronize(SHORT_TIMEOUT).ok());
    ASSERT_EQ(sync.slots_in_use(), MAX_REQUEST_ATTEMPTS);

    // Land one lookup, which puts exactly one slot back. The next call can
    // start, but cannot reach its second attempt.
    transport.deliver_resolution(FakeNtpTransport::address(1));
    ASSERT_EQ(sync.slots_in_use(), MAX_REQUEST_ATTEMPTS - 1);

    const std::size_t opens = transport.open_calls();
    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::TIMED_OUT);
    EXPECT_EQ(transport.open_calls(), opens + 1);
}

TEST(NtpSynchronizer, AnAbandonedLookupReleasesItsSlotWhenItLands) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::IN_PROGRESS);
    transport.resolve_on_poll(0, FakeNtpTransport::address(1));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    ASSERT_FALSE(sync.synchronize(SHORT_TIMEOUT).ok());
    ASSERT_EQ(sync.slots_in_use(), MAX_REQUEST_ATTEMPTS);

    // The late resolution callback releases the abandoned slot.
    for (std::size_t i = 0; i < MAX_REQUEST_ATTEMPTS; ++i) {
        transport.deliver_resolution(FakeNtpTransport::address(1));
    }

    EXPECT_EQ(sync.slots_in_use(), 0U);
    EXPECT_FALSE(transport.resolution_outstanding());

    // Releasing the abandoned slot makes exhaustion transient (AR-6).
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    transport.arrive_on_poll(transport.poll_calls() + 1,
                             FakeNtpTransport::address(1),
                             good_reply(transport.next_nonce()));
    EXPECT_TRUE(sync.synchronize(GENEROUS_TIMEOUT).ok());
}

TEST(NtpSynchronizer, ALateResolutionDoesNotDisturbTheNextAttempt) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::IN_PROGRESS);
    transport.resolve_on_poll(0, FakeNtpTransport::address(1));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    // First call abandons one lookup per attempt.
    ASSERT_FALSE(sync.synchronize(SHORT_TIMEOUT).ok());

    // Release one slot by letting its lookup land, then run again. The freed
    // slot is reused, and the still-outstanding lookups must not write into it.
    transport.deliver_resolution(FakeNtpTransport::address(1));
    ASSERT_EQ(sync.slots_in_use(), MAX_REQUEST_ATTEMPTS - 1);

    transport.set_resolve_result(ResolveStatus::RESOLVED);
    transport.arrive_on_poll(transport.poll_calls() + 1,
                             FakeNtpTransport::address(1),
                             good_reply(transport.next_nonce()));

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);
    ASSERT_TRUE(result.ok()) << describe(result.error());
    EXPECT_EQ(result.value().epoch_seconds, EPOCH_SECONDS);
}

TEST(NtpSynchronizer, AResolutionLandingUnderTheLockIsSerialized) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::IN_PROGRESS);
    // Queue the callback until unlock, after resolve_pending is set.
    // Inline delivery would leave a slot waiting for a callback that already
    // ran.
    transport.resolve_lands_under_the_lock(FakeNtpTransport::address(1));
    transport.arrive_on_poll(1, FakeNtpTransport::address(1),
                             good_reply(transport.next_nonce()));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);

    ASSERT_TRUE(result.ok()) << describe(result.error());
    EXPECT_EQ(transport.open_calls(), 1U);
    EXPECT_EQ(sync.slots_in_use(), 0U);
}

TEST(NtpSynchronizer, EveryErrorHasADescription) {
    constexpr std::array<NtpError, 13> ALL_ERRORS = {
        NtpError::NONE,
        NtpError::INVALID_TIMEOUT,
        NtpError::NO_ATTEMPT_SLOT,
        NtpError::DNS_FAILED,
        NtpError::SOCKET_FAILED,
        NtpError::SEND_FAILED,
        NtpError::TIMED_OUT,
        NtpError::REJECTED_MODE,
        NtpError::REJECTED_LEAP_ALARM,
        NtpError::REJECTED_STRATUM,
        NtpError::REJECTED_TIMESTAMP_ZERO,
        NtpError::REJECTED_TIMESTAMP_IMPLAUSIBLE,
        NtpError::CLOCK_REFUSED,
    };
    for (const NtpError error : ALL_ERRORS) {
        EXPECT_STRNE(describe(error), "unknown") << static_cast<int>(error);
    }
}

// Independent expectations cover every error classification.
TEST(NtpSynchronizer, EveryErrorIsClassifiedForRetry) {
    struct Classification {
        NtpError error;
        bool retryable;
    };

    constexpr std::array<Classification, 14> EXPECTED = {{
        // Success, and a caller's own mistake: retrying changes neither.
        {.error = NtpError::NONE, .retryable = false},
        {.error = NtpError::INVALID_TIMEOUT, .retryable = false},
        // Transient: a pool or a link that clears on its own.
        {.error = NtpError::NO_ATTEMPT_SLOT, .retryable = true},
        {.error = NtpError::DNS_FAILED, .retryable = true},
        {.error = NtpError::SOCKET_FAILED, .retryable = true},
        {.error = NtpError::SEND_FAILED, .retryable = true},
        {.error = NtpError::TIMED_OUT, .retryable = true},
        // Reply rejections are non-retryable.
        {.error = NtpError::REJECTED_MODE, .retryable = false},
        {.error = NtpError::REJECTED_LEAP_ALARM, .retryable = false},
        {.error = NtpError::REJECTED_STRATUM, .retryable = false},
        {.error = NtpError::REJECTED_TIMESTAMP_ZERO, .retryable = false},
        {.error = NtpError::REJECTED_TIMESTAMP_IMPLAUSIBLE, .retryable = false},
        {.error = NtpError::CLOCK_REFUSED, .retryable = false},
    }};
    for (const Classification expected : EXPECTED) {
        EXPECT_EQ(retryable(expected.error), expected.retryable)
            << static_cast<int>(expected.error);
    }
}

}  // namespace
}  // namespace pico_time
