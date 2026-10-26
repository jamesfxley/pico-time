// SPDX-License-Identifier: MIT
// The attempt loop and the slot pool, driven against a scripted transport: the
// NR-3 retry structure, the AR-2 argument refusal, the AR-6 slot-exhaustion
// return, and the release protocol for a lookup that lands after the attempt
// which started it was torn down. The last is the subtlest code in this library
// and is reachable only from here -- a run on hardware exercises one
// interleaving and cannot induce it.
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

// Named rather than spelled with a chrono literal at each call: the literal
// needs a using-directive this file would otherwise carry a suppression for,
// and the timeout is scaffolding in most of these tests rather than the case
// under test.
constexpr auto SHORT_TIMEOUT = std::chrono::milliseconds(200);
constexpr auto GENEROUS_TIMEOUT = std::chrono::milliseconds(1000);
constexpr auto NO_TIMEOUT = std::chrono::milliseconds(0);

// Where a 32-bit millisecond counter returns to zero.
constexpr std::uint64_t CLOCK_WRAP_32 = 0x100000000U;

// Offsets and constants of the SNTP wire format, spelled here rather than
// reached from the module's private header: a test that builds its input with
// the same code that parses it cannot detect a change in either.
constexpr std::size_t MESSAGE_LENGTH = 48;
constexpr std::size_t ORIGINATE_OFFSET = 24;
constexpr std::size_t TRANSMIT_OFFSET = 40;
constexpr std::uint32_t NTP_ERA_OFFSET = 2208988800U;
constexpr std::uint64_t EPOCH_SECONDS = 1767225600U;  // 2026-01-01Z

/// A well-formed server reply to `nonce`, inside the plausibility window VR-6
/// enforces so the decoded value is checkable rather than merely present.
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

/// Arrange a transport whose lookup answers from cache and whose reply lands on
/// the first poll -- the shape every test that is not about failure starts
/// from. By reference rather than returned: a fake is registered with the
/// policy by reference, so it is not copyable or movable.
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
    // Closed exactly once, and left closed. That the close happens before the
    // caller reads the sample is a property of run_attempt's ordering, not one
    // these assertions establish: this fake delivers only from poll(), so it
    // cannot place a second datagram after the close even in principle.
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

// CXX-08.6. The counter a real transport reads is 32 bits wide, so an attempt
// started within its timeout of the wrap has an absolute deadline the counter
// can never reach. What that costs is not a late timeout but no timeout at all:
// the wait for a reply that never comes is the one this library exists to
// bound, and it is exactly the one that would never end.
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
    // An exhausted UDP pcb pool, not a refusal: it clears when whatever else
    // in the image is holding the pool lets go.
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
    // Momentarily out of pbufs is a condition to retry, not to give up on.
    EXPECT_TRUE(retryable(result.error()));
}

// A rejection does not stop the loop: pool.ntp.org resolves to a different
// server each lookup, so the next attempt asks a different machine. What
// changes is what the caller is told when every attempt is refused -- a server
// that reported its own state, rather than a link that went quiet.
TEST(NtpSynchronizer, ARefusedReplyReportsWhyRatherThanTimingOut) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    // Replies that echo each attempt's nonce, so each is that exchange's, but
    // declare themselves a client rather than a server (VR-1).
    for (std::size_t attempt = 1; attempt <= MAX_REQUEST_ATTEMPTS; ++attempt) {
        std::vector<std::uint8_t> reply = good_reply(attempt);
        reply[0] = 0x23;  // mode 3
        transport.arrive_on_poll(attempt, FakeNtpTransport::address(1), reply);
    }
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::REJECTED_MODE);
    // A refused reply will not clear on its own, so a caller retrying in a loop
    // is wasting its time.
    EXPECT_FALSE(retryable(result.error()));
    EXPECT_EQ(transport.send_calls(), MAX_REQUEST_ATTEMPTS);
}

// The counterpart: one refused reply does not condemn the call, because the
// next attempt reaches a different member of the pool.
TEST(NtpSynchronizer, ARejectionOnOneAttemptDoesNotEndTheCall) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::RESOLVED);
    std::vector<std::uint8_t> refused = good_reply(1);
    refused[1] = 0;  // stratum 0: Kiss-o'-Death (VR-3)
    transport.arrive_on_poll(1, FakeNtpTransport::address(1), refused);
    transport.arrive_on_poll(2, FakeNtpTransport::address(1), good_reply(2));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);

    ASSERT_TRUE(result.ok()) << describe(result.error());
    EXPECT_EQ(result.value().epoch_seconds, EPOCH_SECONDS);
    EXPECT_EQ(transport.send_calls(), 2U);
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
    // VR-7. From the server, but one byte short of a message: dropped, so the
    // attempt is still waiting when the whole reply arrives.
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
    // VR-7. A replay carrying someone else's nonce must not end an attempt
    // that would otherwise have succeeded, so the good reply behind it wins.
    transport.arrive_on_poll(1, FakeNtpTransport::address(1), good_reply(999));
    transport.arrive_on_poll(2, FakeNtpTransport::address(1),
                             good_reply(transport.next_nonce()));
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(GENEROUS_TIMEOUT);

    ASSERT_TRUE(result.ok()) << describe(result.error());
    EXPECT_EQ(transport.send_calls(), 1U);
}

// ---------------------------------------------------------------------------
// The slot pool. These are the cases the seam exists for.
// ---------------------------------------------------------------------------

TEST(NtpSynchronizer, ALookupThatNeverLandsHoldsItsSlot) {
    FakeNtpTransport transport;
    transport.set_resolve_result(ResolveStatus::IN_PROGRESS);
    transport.resolve_on_poll(0, FakeNtpTransport::address(1));  // never
    NtpSynchronizer sync(transport, "pool.ntp.org");

    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::TIMED_OUT);
    // Every attempt timed out inside resolution, so every slot was abandoned to
    // a callback that has not fired. This is the state AR-6 describes.
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

    // AR-6: the next call has no slot to claim. It must say so rather than
    // report a network failure, and must not spend three attempts finding out.
    const std::size_t opens = transport.open_calls();
    const auto result = sync.synchronize(SHORT_TIMEOUT);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error(), NtpError::NO_ATTEMPT_SLOT);
    EXPECT_EQ(transport.open_calls(), opens);
    // Slot exhaustion clears on its own.
    EXPECT_TRUE(retryable(result.error()));
}

// AR-6 promises NO_ATTEMPT_SLOT without network traffic and without spending
// an attempt. A pool that still had a slot on entry cannot honour that promise
// once it empties partway through: an endpoint has been opened and a lookup
// issued, and what the caller needs is the diagnosis those produced rather
// than a report about the pool.
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

    // The lookup lwIP could not cancel finally fires, long after the attempt
    // that started it was torn down. Whichever of the caller and the callback
    // runs last releases the slot; here it is the callback.
    for (std::size_t i = 0; i < MAX_REQUEST_ATTEMPTS; ++i) {
        transport.deliver_resolution(FakeNtpTransport::address(1));
    }

    EXPECT_EQ(sync.slots_in_use(), 0U);
    EXPECT_FALSE(transport.resolution_outstanding());

    // And the pool is usable again, which is what makes AR-6 transient.
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
    // Land it inside resolve(), while run_attempt still holds the lock -- the
    // one delivery point that reaches the queue, and the interleaving the lock
    // is taken for. Firing it inline instead would set address_ready before
    // the mainline sets resolve_pending, leaving a slot whose only callback
    // has already run and which nothing is left to release.
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

// The companion to the test above, over the same vocabulary. retryable() is
// what docs/usage-guide.md tells a caller to branch on, so an enumerator
// nobody classified is a caller giving up on a condition that was about to
// clear. The expectations are written out rather than derived from the
// predicate: a table that recomputes what it is checking asserts nothing.
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
        // A server reporting its own state; it reports the same again.
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
