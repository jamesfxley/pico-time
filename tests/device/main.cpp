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
constexpr std::int64_t IRQ_INTERVAL_US = 1;
constexpr std::uint32_t IRQ_DRAIN_US = 100;
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

[[nodiscard]] OffsetRange set_clock(std::uint64_t seconds,
                                    std::atomic<bool>* in_progress = nullptr) {
    const auto before = uptime_us();
    // Release/acquire keeps the marker around the public setter, so an IRQ
    // observing true establishes the coverage precondition for this call.
    if (in_progress != nullptr) {
        in_progress->store(true, std::memory_order_release);
    }
    const bool applied =
        pico_time::apply_wall_clock_time(
            seconds, static_cast<std::uint32_t>(SET_FRACTION_US))
            .ok();
    if (in_progress != nullptr) {
        in_progress->store(false, std::memory_order_release);
    }
    const auto after = uptime_us();
    const auto epoch_us =
        (static_cast<std::int64_t>(seconds) * MICROSECONDS_PER_SECOND) +
        SET_FRACTION_US;
    return {.lower = epoch_us - after,
            .upper = epoch_us - before,
            .ok = applied && pico_time::is_time_synchronized()};
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

    void append_number(int number) {
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
    std::array<ClockRead, IRQ_CAPACITY> samples{};
    std::atomic<std::size_t> count{0};
    std::atomic<bool> overflow{false};
    std::atomic<bool> in_progress{false};
    std::atomic<bool> interrupted{false};
};

static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

// pico/time.h documents repeating-timer callbacks in the default alarm
// pool's IRQ on core 0. Only the callback writes samples. Release/acquire
// publishes completed entries; cancellation on this same core ends writing
// before mainline inspects them. No output or allocation runs in the IRQ.
bool read_in_interrupt(repeating_timer_t* timer) {
    auto& reads = *static_cast<InterruptReads*>(timer->user_data);
    if (reads.in_progress.load(std::memory_order_acquire)) {
        // Independent coverage flag; sample data is published through count.
        reads.interrupted.store(true, std::memory_order_relaxed);
    }
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
// bracket. Each setter call must be preempted while its marker is active.
// This establishes overlap with the public call, not an internal instruction.
struct IrqResult {
    bool passed = false;
    std::string_view detail;
};

[[nodiscard]] IrqResult irq_reads_survive(OffsetRange initial) {
    static InterruptReads reads;
    std::array<OffsetRange, OFFSET_COUNT> offsets{initial};
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        sleep_ms(WRITER_INTERVAL_MS);
        // No callback is active here. This flag carries no sample data.
        reads.interrupted.store(false, std::memory_order_relaxed);
        repeating_timer_t timer{};
        // pico/time.h:801-818: a positive microsecond delay schedules from
        // callback completion, leaving mainline time between IRQ callbacks.
        if (!add_repeating_timer_us(IRQ_INTERVAL_US, read_in_interrupt, &reads,
                                    &timer)) {
            return {.detail = "could not start store interrupt"};
        }
        sleep_us(IRQ_DRAIN_US);
        offsets[index] = set_clock(
            pico_time::MIN_EPOCH_SECONDS + (index * EPOCH_STEP_SECONDS),
            &reads.in_progress);
        sleep_us(IRQ_DRAIN_US);
        if (!cancel_repeating_timer(&timer)) {
            return {.detail = "store interrupt timer did not remain active"};
        }
        // Cancellation on the callback's core ends all writes. This flag
        // carries no sample data; count's release/acquire publishes those.
        if (!reads.interrupted.load(std::memory_order_relaxed)) {
            return {.detail = "a store was not interrupted"};
        }
    }
    const auto count = reads.count.load(std::memory_order_acquire);
    // Independent failure flag; no data is acquired through it.
    bool passed = !reads.overflow.load(std::memory_order_relaxed) &&
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
    return {.passed = passed,
            .detail = passed ? "each store interrupted; whole offsets observed"
                             : "IRQ reads did not match every stored offset"};
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
              irq.detail);
    run.finish();
}
