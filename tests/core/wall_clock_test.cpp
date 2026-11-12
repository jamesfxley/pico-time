// SPDX-License-Identifier: MIT
// Clock arithmetic (FR-1..FR-5) and publication (FR-6).

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

// Offset calculation and time bounds (FR-1, FR-5).

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
    // Microseconds supplied as seconds would overflow an unbounded conversion.
    constexpr std::uint64_t MICROSECONDS_SINCE_EPOCH = 1767225600000000ULL;
    EXPECT_FALSE(
        compute_epoch_offset(MICROSECONDS_SINCE_EPOCH, 0U, BOOT_UPTIME_US)
            .has_value());

    // This value exceeds the signed 64-bit conversion range.
    constexpr std::uint64_t OVERFLOWS_INT64 = 9'223'372'036'855ULL;
    EXPECT_FALSE(
        compute_epoch_offset(OVERFLOWS_INT64, 0U, BOOT_UPTIME_US).has_value());
}

// Offset application and non-negative clamp (FR-1, FR-3).

TEST(ApplyOffset, AddsTheOffsetToTheMonotonicReading) {
    EXPECT_EQ(apply_offset(BOOT_UPTIME_US, MICROSECONDS_PER_SECOND),
              BOOT_UPTIME_US + MICROSECONDS_PER_SECOND);
}

TEST(ApplyOffset, ClampsANegativeResultToTheEpoch) {
    // A negative adjusted time exercises the clamp directly.
    EXPECT_EQ(apply_offset(BOOT_UPTIME_US, -BOOT_UPTIME_US * 2), 0);
    EXPECT_EQ(apply_offset(0, -1), 0);
}

// Offset publication (FR-6).

TEST(WallClock, StartsUnsynchronized) {
    const WallClock clock;
    EXPECT_FALSE(clock.synchronized());
    EXPECT_EQ(clock.load().value_or(-1), 0);
}

// The sync latch stays true after the first store (FR-4).
TEST(WallClock, PublishesAnOffsetAndLatchesSynchronized) {
    WallClock clock;
    constexpr std::int64_t OFFSET = 1'767'225'600'000'000;

    clock.store(OFFSET);

    EXPECT_TRUE(clock.synchronized());
    EXPECT_EQ(clock.load().value_or(-1), OFFSET);
}

TEST(WallClock, CarriesTheFullSignedRangeAcrossTheSplit) {
    // Non-zero bits in both halves and negative values expose reassembly
    // errors.
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

/// A counter paired with its 32-bit complement detects mixed stores.
[[nodiscard]] std::int64_t self_checking(std::uint32_t counter) {
    // Complementing in 32 bits before widening preserves the intended high
    // half.
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
    // Distinct self-checking values expose mixtures of different stores.
    // The writer yields to reduce the chance of lapping a descheduled reader.
    // Yielding does not enforce the one-store-per-second writer contract.
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
                // Only successful reads are validated.
                // Exhaustion returns no value.
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
