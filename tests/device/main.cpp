// SPDX-License-Identifier: MIT
#include <hardware/timer.h>
#include <pico/stdio.h>
#include <pico/time.h>
#include <sys/_timeval.h>
#include <sys/time.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <pico_family/device_test.hpp>
#include <pico_time/core/epoch.hpp>
#include <pico_time/time_sync/time_sync.hpp>

namespace {

constexpr std::int64_t MICROSECONDS_PER_SECOND = 1'000'000;
constexpr std::uint32_t WRITER_INTERVAL_MS = 1000;
constexpr std::uint32_t YIELD_MS = 1;
constexpr std::size_t ADVANCE_READS = 8;
constexpr std::int32_t IRQ_INTERVAL_MS = 10;
constexpr std::uint32_t IRQ_DRAIN_MS = 20;
constexpr std::size_t IRQ_CAPACITY = 256;
constexpr std::size_t OFFSET_COUNT = 3;
constexpr std::uint64_t EPOCH_STEP_SECONDS = 86400;
constexpr std::int64_t SET_FRACTION_US = 123456;

struct ClockRead {
    std::int64_t before = 0;
    std::int64_t after = 0;
    std::int64_t value = 0;
    bool ok = false;
};

struct OffsetRange {
    std::int64_t lower = 0;
    std::int64_t upper = 0;
    bool ok = false;
};

[[nodiscard]] std::int64_t uptime_us() {
    return static_cast<std::int64_t>(time_us_64());
}

[[nodiscard]] ClockRead read_clock() {
    timeval value{};
    const auto before = uptime_us();
    const int status = gettimeofday(&value, nullptr);
    const auto after = uptime_us();
    return {.before = before,
            .after = after,
            .value = (static_cast<std::int64_t>(value.tv_sec) *
                      MICROSECONDS_PER_SECOND) +
                     value.tv_usec,
            .ok = status == 0 && value.tv_usec >= 0 &&
                  value.tv_usec < MICROSECONDS_PER_SECOND};
}

[[nodiscard]] bool matches_offset(const ClockRead& sample, OffsetRange offset) {
    return sample.ok && offset.ok &&
           sample.value >= sample.before + offset.lower &&
           sample.value <= sample.after + offset.upper;
}

// FR-2: each interface is bracketed separately, including its own rounding.
[[nodiscard]] bool clocks_start_at_uptime() {
    const auto sample = read_clock();
    const auto before_time = uptime_us() / MICROSECONDS_PER_SECOND;
    const auto seconds = std::time(nullptr);
    const auto after_time = uptime_us() / MICROSECONDS_PER_SECOND;
    const auto before_chrono = uptime_us();
    const auto chrono_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto after_chrono = uptime_us();
    return !pico_time::is_time_synchronized() &&
           matches_offset(sample, {.lower = 0, .upper = 0, .ok = true}) &&
           seconds >= before_time && seconds <= after_time &&
           chrono_us >= before_chrono && chrono_us <= after_chrono;
}

[[nodiscard]] OffsetRange set_clock(std::uint64_t seconds) {
    const auto before = uptime_us();
    const bool applied =
        pico_time::apply_wall_clock_time(
            seconds, static_cast<std::uint32_t>(SET_FRACTION_US))
            .ok();
    const auto after = uptime_us();
    const auto epoch_us =
        (static_cast<std::int64_t>(seconds) * MICROSECONDS_PER_SECOND) +
        SET_FRACTION_US;
    return {.lower = epoch_us - after,
            .upper = epoch_us - before,
            .ok = applied && pico_time::is_time_synchronized()};
}

// FR-7: no writer runs between these reads; yields exercise the newlib shim.
[[nodiscard]] bool clock_advances(OffsetRange offset) {
    const auto first = read_clock();
    auto previous = first;
    bool passed =
        matches_offset(first, offset) && pico_time::is_time_synchronized();
    for (std::size_t index = 0; index < ADVANCE_READS; ++index) {
        sleep_ms(YIELD_MS);
        const auto next = read_clock();
        passed = matches_offset(next, offset) && next.value >= previous.value &&
                 passed;
        previous = next;
    }
    return passed && previous.value > first.value;
}

struct InterruptReads {
    std::array<ClockRead, IRQ_CAPACITY> samples{};
    std::atomic<std::size_t> count{0};
    std::atomic<bool> overflow{false};
};

static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

// pico/time.h documents repeating-timer callbacks in the default alarm
// pool's IRQ on core 0. Only the callback writes samples. Release/acquire
// publishes completed entries; cancellation on this same core ends writing
// before mainline inspects them. No output or allocation runs in the IRQ.
bool read_in_interrupt(repeating_timer_t* timer) {
    auto& reads = *static_cast<InterruptReads*>(timer->user_data);
    // This count has one writer and is not used to acquire sample contents.
    const auto index = reads.count.load(std::memory_order_relaxed);
    if (index == reads.samples.size()) {
        // Independent failure flag; it publishes no other data.
        reads.overflow.store(true, std::memory_order_relaxed);
        return false;
    }
    reads.samples[index] = read_clock();
    reads.count.store(index + 1, std::memory_order_release);
    return true;
}

// FR-6: three widely separated offsets exercise both halves of the offset.
// Every IRQ sample must match a complete published offset and its own uptime
// bracket. This workload does not claim to force preemption at a store.
[[nodiscard]] bool irq_reads_survive(OffsetRange initial) {
    static InterruptReads reads;
    std::array<OffsetRange, OFFSET_COUNT> offsets{initial};
    repeating_timer_t timer{};
    sleep_ms(WRITER_INTERVAL_MS);
    if (!add_repeating_timer_ms(IRQ_INTERVAL_MS, read_in_interrupt, &reads,
                                &timer)) {
        return false;
    }
    sleep_ms(IRQ_DRAIN_MS);
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        offsets[index] = set_clock(pico_time::MIN_EPOCH_SECONDS +
                                   (index * EPOCH_STEP_SECONDS));
        sleep_ms(WRITER_INTERVAL_MS);
    }
    const bool cancelled = cancel_repeating_timer(&timer);
    const auto count = reads.count.load(std::memory_order_acquire);
    // Independent failure flag; no data is acquired through it.
    bool passed = cancelled &&
                  !reads.overflow.load(std::memory_order_relaxed) &&
                  count > 0 && count <= reads.samples.size();
    std::array<bool, OFFSET_COUNT> observed{};
    for (std::size_t index = 0; index < count && index < reads.samples.size();
         ++index) {
        bool whole = false;
        for (std::size_t offset = 0; offset < offsets.size(); ++offset) {
            const bool matches =
                matches_offset(reads.samples[index], offsets[offset]);
            observed[offset] = observed[offset] || matches;
            whole = whole || matches;
        }
        passed = whole && passed;
    }
    for (const bool seen : observed) {
        passed = seen && passed;
    }
    return passed;
}

}  // namespace

int main() {
    stdio_init_all();
    pico_family::device_test::Run run;
    run.check("FR-2", "newlib-clocks-start-at-uptime", clocks_start_at_uptime(),
              "boot brackets and unsynchronized state");
    const auto initial = set_clock(pico_time::MIN_EPOCH_SECONDS);
    run.check("FR-7", "newlib-clock-advances-without-resetting-offset",
              clock_advances(initial),
              "advances across yields with fixed offset");
    run.check("FR-6", "irq-clock-read-survives-mainline-set",
              irq_reads_survive(initial),
              "IRQ reads match whole stored offsets");
    run.finish();
}
