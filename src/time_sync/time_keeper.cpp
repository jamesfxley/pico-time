// SPDX-License-Identifier: MIT
// Newlib hooks override the SDK clock. Portable arithmetic is in
// wall_clock.hpp.

#include <hardware/timer.h>
#include <sys/time.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <pico_time/ntp/ntp_error.hpp>
#include <pico_time/time_sync/time_sync.hpp>
#include <pico_util/result/result.hpp>
#include <utility>

#include "pico_time/pico_time.hpp"
#include "wall_clock.hpp"

static_assert(pico_time::VERSION_MAJOR == PICO_TIME_CMAKE_VERSION_MAJOR);
static_assert(pico_time::VERSION_MINOR == PICO_TIME_CMAKE_VERSION_MINOR);
static_assert(pico_time::VERSION_PATCH == PICO_TIME_CMAKE_VERSION_PATCH);

namespace {

constexpr std::int64_t MICROSECONDS_PER_SECOND = 1'000'000;

std::int64_t monotonic_time_us() {
    return static_cast<std::int64_t>(time_us_64());
}

}  // namespace

// Before synchronisation, _gettimeofday reports time since boot.
// Afterwards, it reports Unix time.
// readability-identifier-naming: the syscall name is fixed by the ABI.
// misc-include-cleaner: timeval uses POSIX sys/time.h, not a newlib internal.
// NOLINTNEXTLINE(readability-identifier-naming,misc-include-cleaner)
extern "C" int _gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    if (tv == nullptr) {
        return 0;
    }

    const pico_time::WallClock& clock = pico_time::wall_clock();
    std::int64_t adjusted = monotonic_time_us();
    if (clock.synchronized()) {
        const auto offset = clock.load();
        if (!offset.ok()) {
            // newlib time() maps a negative return to (time_t)-1. EAGAIN says a
            // later read can succeed.
            errno = EAGAIN;
            return -1;
        }
        adjusted = pico_time::apply_offset(adjusted, offset.value());
    }

    tv->tv_sec = adjusted / MICROSECONDS_PER_SECOND;
    // The cast narrows to suseconds_t on this target.
    // misc-include-cleaner: POSIX sys/time.h supplies suseconds_t.
    // NOLINTNEXTLINE(misc-include-cleaner)
    tv->tv_usec = static_cast<suseconds_t>(adjusted % MICROSECONDS_PER_SECOND);
    return 0;
}

// readability-identifier-naming: the syscall name is fixed by the ABI.
// misc-include-cleaner names newlib sys/_timeval.h, an implementation header
// no portable source should include. POSIX supplies timeval through sys/time.h.
// NOLINTNEXTLINE(readability-identifier-naming,misc-include-cleaner)
extern "C" int _settimeofday(const struct timeval* tv,
                             const struct timezone* tz) {
    (void)tz;
    if (tv == nullptr) {
        errno = EINVAL;
        return -1;
    }

    // Negative fields are rejected before unsigned conversion (AR-4).
    // cmp_greater compares signed tv_usec to the unsigned bound without a cast.
    if (tv->tv_sec < 0 || tv->tv_usec < 0 ||
        std::cmp_greater(tv->tv_usec, pico_time::MAX_MICROSECONDS)) {
        errno = EINVAL;
        return -1;
    }

    if (!pico_time::apply_wall_clock_time(
            static_cast<std::uint64_t>(tv->tv_sec),
            static_cast<std::uint32_t>(tv->tv_usec))) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

// The SDK weak setter writes its own offset. This override shares
// _settimeofday validation (AR-4).
extern "C" int settimeofday(const struct timeval* tv,
                            const struct timezone* tz) {
    return _settimeofday(tv, tz);
}

namespace pico_time {

bool is_time_synchronized() { return wall_clock().synchronized(); }

pico_util::Result<void, WallClockError> apply_wall_clock_time(
    std::uint64_t epoch_seconds, std::uint32_t epoch_microseconds) {
    // Separate checks identify which bound failed (AR-3).
    if (epoch_microseconds > MAX_MICROSECONDS) {
        return WallClockError::MICROSECONDS_INVALID;
    }
    if (epoch_seconds < MIN_EPOCH_SECONDS ||
        epoch_seconds >= MAX_EPOCH_SECONDS) {
        return WallClockError::EPOCH_OUT_OF_RANGE;
    }

    const std::optional<std::int64_t> offset = compute_epoch_offset(
        epoch_seconds, epoch_microseconds, monotonic_time_us());
    if (!offset.has_value()) {
        // Defensive check in case compute_epoch_offset() gains another failure
        // path.
        return WallClockError::EPOCH_OUT_OF_RANGE;
    }

    wall_clock().store(*offset);
    return {};
}

}  // namespace pico_time
