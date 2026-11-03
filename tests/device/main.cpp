// SPDX-License-Identifier: MIT
#include <hardware/timer.h>
#include <pico/stdio.h>
#include <pico/time.h>
#include <sys/_timeval.h>
#include <sys/time.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <pico_family/device_test.hpp>
#include <pico_time/core/epoch.hpp>
#include <pico_time/time_sync/time_sync.hpp>
#include <string_view>
#include <system_error>

namespace {

constexpr std::int64_t MICROSECONDS_PER_SECOND = 1'000'000;
constexpr std::uint32_t WRITER_INTERVAL_MS = 1000;
constexpr std::uint32_t YIELD_MS = 1;
constexpr std::size_t ADVANCE_READS = 8;
constexpr std::uint64_t IRQ_WINDOW_US = 2000;
constexpr std::int64_t IRQ_DELAY_US = 10;
constexpr std::uint64_t IRQ_SIDE_WINDOW_US = 100;
constexpr std::size_t IRQ_STORES = 3;
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
    const auto applied = pico_time::apply_wall_clock_time(
        seconds, static_cast<std::uint32_t>(SET_FRACTION_US));
    const auto after = uptime_us();
    const auto epoch_us =
        (static_cast<std::int64_t>(seconds) * MICROSECONDS_PER_SECOND) +
        SET_FRACTION_US;
    return {.lower = epoch_us - after,
            .upper = epoch_us - before,
            .ok = applied.ok() && pico_time::is_time_synchronized()};
}

class FailureDetail {
   public:
    void append(std::string_view text) {
        if (text.size() > buffer_.size() - used_) {
            complete_ = false;
            return;
        }
        for (const char character : text) {
            buffer_[used_] = character;
            ++used_;
        }
    }

    void append_number(std::int64_t number) {
        std::array<char, 32> digits{};
        const auto converted =
            std::to_chars(digits.begin(), digits.end(), number);
        if (converted.ec != std::errc{}) {
            complete_ = false;
            return;
        }
        append({digits.data(),
                static_cast<std::size_t>(converted.ptr - digits.data())});
    }

    [[nodiscard]] std::string_view text() const {
        return complete_ ? std::string_view{buffer_.data(), used_}
                         : "failure detail exceeded fixed capacity";
    }

   private:
    std::array<char, 256> buffer_{};
    std::size_t used_ = 0;
    bool complete_ = true;
};

struct RefusalResult {
    bool passed = true;
    FailureDetail detail;
};

// AR-4: record the first unexpected outcome on the promised POSIX surface.
void inspect_posix_set(RefusalResult& result, const timeval& value,
                       const char* phase, const char* input, bool valid) {
    const bool synchronized = pico_time::is_time_synchronized();
    const auto before = read_clock();
    errno = 0;
    const auto call_before = uptime_us();
    const int status = settimeofday(&value, nullptr);
    const int error = errno;
    const auto call_after = uptime_us();
    const auto after = read_clock();
    const OffsetRange original{.lower = before.value - before.after,
                               .upper = before.value - before.before,
                               .ok = before.ok};
    const bool moved = !matches_offset(after, original);
    const bool state = pico_time::is_time_synchronized();
    const auto epoch_us =
        (static_cast<std::int64_t>(value.tv_sec) * MICROSECONDS_PER_SECOND) +
        value.tv_usec;
    const bool applied = matches_offset(after, {.lower = epoch_us - call_after,
                                                .upper = epoch_us - call_before,
                                                .ok = status == 0});
    const bool passed = valid ? status == 0 && state && applied
                              : status == -1 && error == EINVAL && !moved &&
                                    state == synchronized;
    if (!passed && result.passed) {
        result.detail.append("first-failure phase=");
        result.detail.append(phase);
        result.detail.append(" input=");
        result.detail.append(input);
        result.detail.append(" return=");
        result.detail.append_number(status);
        result.detail.append(" errno=");
        result.detail.append_number(error);
        result.detail.append(" pico-clock-moved=");
        if (before.ok && after.ok) {
            result.detail.append(moved ? "yes" : "no");
        } else {
            result.detail.append("unreadable");
        }
        result.detail.append(" sync-state-changed=");
        result.detail.append(state != synchronized ? "yes" : "no");
        result.detail.append(" input-offset-applied=");
        result.detail.append(applied ? "yes" : "no");
    }
    result.passed = passed && result.passed;
}

// AR-4: all invalid inputs run before and after one valid POSIX set.
[[nodiscard]] RefusalResult refuses_invalid_inputs() {
    struct Input {
        const char* name = nullptr;
        timeval value{};
    };

    const auto floor = static_cast<time_t>(pico_time::MIN_EPOCH_SECONDS);
    const auto ceiling = static_cast<time_t>(pico_time::MAX_EPOCH_SECONDS);
    const std::array inputs{
        Input{.name = "negative-seconds",
              .value = {.tv_sec = -1, .tv_usec = 0}},
        Input{.name = "negative-microseconds",
              .value = {.tv_sec = floor, .tv_usec = -1}},
        Input{.name = "million-microseconds",
              .value = {.tv_sec = floor, .tv_usec = MICROSECONDS_PER_SECOND}},
        Input{.name = "below-window",
              .value = {.tv_sec = floor - 1, .tv_usec = 0}},
        Input{.name = "at-ceiling",
              .value = {.tv_sec = ceiling, .tv_usec = 0}}};
    RefusalResult result;
    for (const auto& input : inputs) {
        inspect_posix_set(result, input.value, "before", input.name, false);
    }
    const timeval valid{.tv_sec = floor, .tv_usec = SET_FRACTION_US};
    inspect_posix_set(result, valid, "valid", "local-set", true);
    for (const auto& input : inputs) {
        inspect_posix_set(result, input.value, "after", input.name, false);
    }
    return result;
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
    // Immutable while the repeating timer is registered. The new clock may
    // advance from new_epoch_us by no more than elapsed uptime since
    // started_us.
    OffsetRange old_offset{};
    std::int64_t new_epoch_us = 0;
    std::uint64_t started_us = 0;
    std::atomic<bool> sampled{false};
    std::atomic<bool> bad_read{false};
    ClockRead first_bad{};
};

static_assert(std::atomic<bool>::is_always_lock_free);

// pico/time.h:739-741: true continues repeating. No buffer or callback limit
// can stop this timer. The default pool dispatches on this mainline core
// (pico/time.h:801-818); cancellation precedes inspecting/resetting the slot.
bool read_in_interrupt(repeating_timer_t* timer) {
    auto& reads = *static_cast<InterruptReads*>(timer->user_data);
    const auto sample = read_clock();
    const bool old = matches_offset(sample, reads.old_offset);
    const auto elapsed =
        sample.after - static_cast<std::int64_t>(reads.started_us);
    const bool fresh = sample.ok && sample.value >= reads.new_epoch_us &&
                       sample.value <= reads.new_epoch_us + elapsed;
    // Only this core's callback writes first_bad; retain the first failure.
    // Release/acquire publishes the fixed slot to mainline after cancellation.
    if (!old && !fresh && !reads.bad_read.load(std::memory_order_acquire)) {
        reads.first_bad = sample;
        reads.bad_read.store(true, std::memory_order_release);
    }
    // Independent coverage flag; no other data is published by this flag.
    reads.sampled.store(true, std::memory_order_relaxed);
    return true;
}

struct IrqResult {
    bool passed = false;
    FailureDetail detail;
};

[[nodiscard]] IrqResult irq_result(bool passed, std::string_view reason,
                                   const InterruptReads* reads = nullptr) {
    IrqResult result;
    result.passed = passed;
    result.detail.append(reason);
    if (reads != nullptr && reads->bad_read.load(std::memory_order_acquire)) {
        result.detail.append(" first-value=");
        result.detail.append_number(reads->first_bad.value);
        result.detail.append(" readable=");
        result.detail.append(reads->first_bad.ok ? "yes" : "no");
    }
    return result;
}

struct IrqStoreResult {
    OffsetRange offset{};
    std::string_view failure;
};

[[nodiscard]] IrqStoreResult capture_irq_store(InterruptReads& reads,
                                               OffsetRange old_offset,
                                               std::uint64_t seconds) {
    // No timer is active while resetting the independent flags/expectations.
    reads.sampled.store(false, std::memory_order_relaxed);
    reads.bad_read.store(false, std::memory_order_release);
    reads.old_offset = old_offset;
    reads.new_epoch_us =
        (static_cast<std::int64_t>(seconds) * MICROSECONDS_PER_SECOND) +
        SET_FRACTION_US;
    reads.started_us = time_us_64();
    repeating_timer_t timer{};
    // pico/time.h:810: a positive delay is from callback completion to the
    // next callback's start. This leaves mainline time between reads without
    // catch-up bursts. The fixed window samples around the store; it does
    // not establish that an IRQ preempted the setter itself.
    if (!add_repeating_timer_us(IRQ_DELAY_US, read_in_interrupt, &reads,
                                &timer)) {
        return {.failure = "could not start store interrupt timer"};
    }
    sleep_us(IRQ_SIDE_WINDOW_US);
    const auto stored = set_clock(seconds);
    sleep_us(IRQ_SIDE_WINDOW_US);
    const bool cancelled = cancel_repeating_timer(&timer);
    if (reads.bad_read.load(std::memory_order_acquire)) {
        return {.failure = "interrupt read was not a whole permitted value"};
    }
    if (!cancelled) {
        return {.failure = "store interrupt timer did not remain active"};
    }
    if (time_us_64() - reads.started_us >= IRQ_WINDOW_US) {
        return {.failure = "store interrupt window deadline expired"};
    }
    // No payload depends on sampled: only callback presence is checked.
    if (!reads.sampled.load(std::memory_order_relaxed)) {
        return {.failure = "no callbacks in store interrupt window"};
    }
    if (!stored.ok) {
        return {.failure = "public setter rejected valid time"};
    }
    return {.offset = stored, .failure = {}};
}

// FR-6: statistical sampling in windows around spaced mainline stores.
// Preemption inside this setter is not a coverage precondition or a claim.
[[nodiscard]] IrqResult irq_reads_survive(OffsetRange initial) {
    if (!initial.ok) {
        return irq_result(false, "initial clock offset was not applied");
    }
    auto previous = initial;
    std::uint64_t seconds = pico_time::MIN_EPOCH_SECONDS;
    InterruptReads reads;
    for (std::size_t index = 0; index < IRQ_STORES; ++index) {
        sleep_ms(WRITER_INTERVAL_MS);
        seconds += EPOCH_STEP_SECONDS;
        const auto capture = capture_irq_store(reads, previous, seconds);
        if (!capture.failure.empty()) {
            return irq_result(false, capture.failure, &reads);
        }
        previous = capture.offset;
    }
    return irq_result(true,
                      "interrupt sampling returned whole permitted values");
}

}  // namespace

int main() {
    stdio_init_all();
    pico_family::device_test::Run run;
    run.check("FR-2", "newlib-clocks-start-at-uptime", clocks_start_at_uptime(),
              "boot brackets and unsynchronized state");
    const auto refusal = refuses_invalid_inputs();
    run.check("AR-4", "settimeofday-refuses-invalid-inputs", refusal.passed,
              refusal.detail.text());
    // A functioning POSIX set writes once; preserve FR-6's writer interval
    // before the independent FR-7/FR-6 baseline is applied.
    sleep_ms(WRITER_INTERVAL_MS);
    const auto initial = set_clock(pico_time::MIN_EPOCH_SECONDS);
    run.check("FR-7", "newlib-clock-advances-without-resetting-offset",
              clock_advances(initial),
              "advances across yields with fixed offset");
    const auto irq = irq_reads_survive(initial);
    run.check("FR-6", "irq-clock-read-survives-mainline-set", irq.passed,
              irq.detail.text());
    run.finish();
}
