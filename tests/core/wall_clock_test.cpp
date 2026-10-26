// SPDX-License-Identifier: MIT
// The clock arithmetic (FR-1..FR-5) and the publication protocol (FR-6).
//
// The arithmetic half is where the overflow lived: apply_wall_clock_time()
// validated microseconds and left the seconds completely unbounded. The
// protocol half is what a device run cannot show -- a torn read needs a
// concurrent writer, and here it gets one.

#include "wall_clock.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <pico_time/core/epoch.hpp>
#include <thread>
#include <vector>

namespace {

using pico_time::apply_offset;
using pico_time::compute_epoch_offset;
using pico_time::MAX_EPOCH_SECONDS;
using pico_time::MIN_EPOCH_SECONDS;
using pico_time::WallClock;

constexpr std::int64_t MICROSECONDS_PER_SECOND = 1'000'000;
constexpr std::uint64_t EPOCH_2026 = 1767225600ULL;
constexpr std::int64_t BOOT_UPTIME_US = 42'000'000;

// --- FR-1: the offset, and FR-5: the bounds -------------------------------

TEST(ComputeEpochOffset, ProducesTheOffsetBetweenEpochAndUptime) {
    const auto offset =
        compute_epoch_offset(EPOCH_2026, 250000U, BOOT_UPTIME_US);

    if (!offset.has_value()) {
        FAIL() << "a valid epoch time was rejected";
        return;
    }
    EXPECT_EQ(*offset, (static_cast<std::int64_t>(EPOCH_2026) *
                        MICROSECONDS_PER_SECOND) +
                           250000 - BOOT_UPTIME_US);
}

TEST(ComputeEpochOffset, RejectsOutOfRangeMicroseconds) {
    EXPECT_FALSE(compute_epoch_offset(EPOCH_2026, 1'000'000U, BOOT_UPTIME_US)
                     .has_value());
    EXPECT_TRUE(
        compute_epoch_offset(EPOCH_2026, 999999U, BOOT_UPTIME_US).has_value());
}

TEST(ComputeEpochOffset, EnforcesTheSecondsWindowAtItsEdges) {
    EXPECT_FALSE(compute_epoch_offset(MIN_EPOCH_SECONDS - 1, 0U, BOOT_UPTIME_US)
                     .has_value());
    EXPECT_TRUE(compute_epoch_offset(MIN_EPOCH_SECONDS, 0U, BOOT_UPTIME_US)
                    .has_value());
    EXPECT_TRUE(compute_epoch_offset(MAX_EPOCH_SECONDS - 1, 0U, BOOT_UPTIME_US)
                    .has_value());
    EXPECT_FALSE(compute_epoch_offset(MAX_EPOCH_SECONDS, 0U, BOOT_UPTIME_US)
                     .has_value());
}

TEST(ComputeEpochOffset, RejectsTheSecondsForMicrosecondsMixUp) {
    // The plausible caller error: microseconds-since-epoch (~1.7e15) passed
    // where seconds were wanted. Unbounded, this multiplied past int64 and was
    // signed overflow -- undefined behaviour, with no UBSan anywhere near it
    // because the library had no host build at all.
    constexpr std::uint64_t MICROSECONDS_SINCE_EPOCH = 1767225600000000ULL;
    EXPECT_FALSE(
        compute_epoch_offset(MICROSECONDS_SINCE_EPOCH, 0U, BOOT_UPTIME_US)
            .has_value());

    // And the value that would overflow the multiply outright.
    constexpr std::uint64_t OVERFLOWS_INT64 = 9'223'372'036'855ULL;
    EXPECT_FALSE(
        compute_epoch_offset(OVERFLOWS_INT64, 0U, BOOT_UPTIME_US).has_value());
}

// --- FR-1: a read adds the offset back, and FR-3: the clamp -----------------

TEST(ApplyOffset, AddsTheOffsetToTheMonotonicReading) {
    EXPECT_EQ(apply_offset(BOOT_UPTIME_US, MICROSECONDS_PER_SECOND),
              BOOT_UPTIME_US + MICROSECONDS_PER_SECOND);
}

TEST(ApplyOffset, ClampsANegativeResultToTheEpoch) {
    // Defence rather than correction: compute_epoch_offset() refuses every
    // input that could get here, so this firing means a defect elsewhere.
    EXPECT_EQ(apply_offset(BOOT_UPTIME_US, -BOOT_UPTIME_US * 2), 0);
    EXPECT_EQ(apply_offset(0, -1), 0);
}

// --- FR-6: the publication protocol ------------------------------------------

TEST(WallClock, StartsUnsynchronized) {
    const WallClock clock;
    EXPECT_FALSE(clock.synchronized());
    EXPECT_EQ(clock.load().value_or(-1), 0);
}

// FR-4. The latch: synchronized() is false until a store lands and true from
// then on, which is what makes synchronize_ntp() a no-op on a set clock.
TEST(WallClock, PublishesAnOffsetAndLatchesSynchronized) {
    WallClock clock;
    constexpr std::int64_t OFFSET = 1'767'225'600'000'000;

    clock.store(OFFSET);

    EXPECT_TRUE(clock.synchronized());
    EXPECT_EQ(clock.load().value_or(-1), OFFSET);
}

TEST(WallClock, CarriesTheFullSignedRangeAcrossTheSplit) {
    // The offset is 64 bits published as two 32-bit halves. A value with bits
    // set in both halves, and a negative one, are what catch a reassembly that
    // drops or sign-extends the wrong half.
    WallClock clock;
    constexpr std::array<std::int64_t, 5> VALUES{{
        0,
        -1,
        0x0000'0001'0000'0000,
        0x7FFF'FFFF'FFFF'FFFF,
        -0x0123'4567'89AB'CDEF,
    }};

    for (const std::int64_t value : VALUES) {
        clock.store(value);
        EXPECT_EQ(clock.load().value_or(-1), value);
    }
}

/// A value whose two halves check each other: the high half is the complement
/// of the low. Any read that mixes the halves of two different published values
/// fails the check, because no two stores share a low half.
[[nodiscard]] std::int64_t self_checking(std::uint32_t counter) {
    // The complement is taken in 32 bits and widened afterwards, in that order
    // and in two statements: complementing inside the cast's argument reads as
    // a 64-bit complement narrowed back down, which is a different value.
    const std::uint32_t complement = ~counter;
    const auto low = static_cast<std::uint64_t>(counter);
    const auto high = static_cast<std::uint64_t>(complement);
    return static_cast<std::int64_t>((high << 32U) | low);
}

[[nodiscard]] bool halves_agree(std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    const auto low = static_cast<std::uint32_t>(bits);
    const auto high = static_cast<std::uint32_t>(bits >> 32U);
    return high == ~low;
}

TEST(WallClock, ReadsAreNeverTornUnderAConcurrentWriter) {
    // The property a device run cannot demonstrate: it needs a writer running
    // concurrently with a reader, which on target means the other core.
    //
    // Every published value carries its own consistency check, so a torn read
    // is detected whichever two stores it mixed. Alternating between two fixed
    // values only catches the mixtures that happen to differ from both, which
    // is a fraction of them.
    //
    // The writer yields between stores. That is not weakening the test: it is
    // testing the design's stated contract. No finite-slot protocol can
    // survive a reader descheduled mid-read while an unpaced writer laps it,
    // and the contract here is one store per second -- against which a yield
    // between stores is still many orders of magnitude more aggressive than
    // anything a successful NTP sync can produce.
    constexpr int READER_COUNT = 2;
    constexpr std::uint32_t ITERATIONS = 50'000;

    WallClock clock;
    clock.store(self_checking(0));

    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> reads{0};
    std::atomic<int> validated{0};

    std::vector<std::thread> readers;
    readers.reserve(READER_COUNT);
    for (int i = 0; i < READER_COUNT; ++i) {
        readers.emplace_back([&clock, &stop, &torn, &reads, &validated]() {
            while (!stop.load(std::memory_order_relaxed)) {
                // A read that could not be validated is reported rather than
                // returned, so only a value reported as a success is checked.
                const auto read = clock.load();
                if (read.ok()) {
                    validated.fetch_add(1, std::memory_order_relaxed);
                    if (!halves_agree(read.value())) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::uint32_t i = 1; i <= ITERATIONS; ++i) {
        clock.store(self_checking(i));
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) {
        reader.join();
    }

    EXPECT_EQ(torn.load(), 0);
    EXPECT_GT(validated.load(), 0);
    // Guards against the test passing because the readers never ran.
    EXPECT_GT(static_cast<std::uint32_t>(reads.load()), ITERATIONS);
}

TEST(WallClock, TheProcessWideClockIsOneObject) {
    EXPECT_EQ(&pico_time::wall_clock(), &pico_time::wall_clock());
}

}  // namespace
