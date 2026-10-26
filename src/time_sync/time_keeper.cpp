// SPDX-License-Identifier: MIT
// The newlib syscall shims that expose the wall clock.
//
// Defining _gettimeofday/_settimeofday here overrides newlib's weak stubs at
// link time, so std::chrono::system_clock, time(), and gettimeofday() pick the
// synchronized clock up with no further wiring.
//
// The clock itself -- the offset, its publication protocol and the arithmetic
// -- lives in wall_clock.hpp, which has no SDK dependency and is host-tested.
// What is left here is the part that genuinely needs the target: reading the
// SDK's monotonic counter, and the ABI-mandated syscall names.

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

// The advertised version and project(pico_time VERSION ...) are two records of
// one fact. Bumping either alone stops the build here.
static_assert(pico_time::VERSION_MAJOR == PICO_TIME_CMAKE_VERSION_MAJOR);
static_assert(pico_time::VERSION_MINOR == PICO_TIME_CMAKE_VERSION_MINOR);
static_assert(pico_time::VERSION_PATCH == PICO_TIME_CMAKE_VERSION_PATCH);

namespace {

constexpr std::int64_t MICROSECONDS_PER_SECOND = 1'000'000;

std::int64_t monotonic_time_us() {
    return static_cast<std::int64_t>(time_us_64());
}

}  // namespace

// Before the first synchronization this reports time since boot; afterwards,
// Unix epoch time.
// Same two reasons as _settimeofday below: an ABI-mandated name, and timeval
// reached through the POSIX <sys/time.h> rather than a newlib internal.
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
            // The offset could not be validated, which happens only when a
            // writer breaks the wall clock's contract. Reported rather than
            // answered with a possibly torn time: newlib's time() turns a
            // negative return into (time_t)-1, the standard failure value, and
            // EAGAIN says a later call may succeed.
            errno = EAGAIN;
            return -1;
        }
        adjusted = pico_time::apply_offset(adjusted, offset.value());
    }

    tv->tv_sec = adjusted / MICROSECONDS_PER_SECOND;
    // suseconds_t reached through the POSIX <sys/time.h>; include-cleaner
    // names a newlib internal header instead. The cast is not redundant --
    // suseconds_t is narrower than int64_t on this target.
    // NOLINTNEXTLINE(misc-include-cleaner)
    tv->tv_usec = static_cast<suseconds_t>(adjusted % MICROSECONDS_PER_SECOND);
    return 0;
}

// A newlib syscall name: the leading underscore is mandated by the ABI, not a
// style choice. struct timeval comes from <sys/time.h>, the POSIX-mandated
// header; include-cleaner names newlib's sys/_timeval.h, an implementation
// header no portable source should include.
// NOLINTNEXTLINE(readability-identifier-naming,misc-include-cleaner)
extern "C" int _settimeofday(const struct timeval* tv,
                             const struct timezone* tz) {
    (void)tz;
    if (tv == nullptr) {
        errno = EINVAL;
        return -1;
    }

    // Validated exactly as apply_wall_clock_time(), and for the same reason:
    // an unchecked tv_usec silently skewed the clock by whole seconds, and a
    // negative tv_sec latched it at the epoch with no way back short of a
    // reboot (AR-4). POSIX specifies EINVAL for a tv_usec out of range.
    // std::cmp_greater rather than a cast: tv_usec is signed and the bound is
    // unsigned, and a cast to silence that is the very pattern that hides a
    // negative value behind a large positive one.
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

namespace pico_time {

bool is_time_synchronized() { return wall_clock().synchronized(); }

pico_util::Result<void, WallClockError> apply_wall_clock_time(
    std::uint64_t epoch_seconds, std::uint32_t epoch_microseconds) {
    // The two bounds are checked here as well as inside compute_epoch_offset()
    // so the caller is told which one it broke. The offset function returns a
    // single nullopt by design -- it has one job and one answer -- and a caller
    // that passed microseconds-since-epoch into the seconds argument cannot
    // find that mistake from a bare failure (AR-3).
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
        // Unreachable while the two checks above mirror the offset function's
        // own. Kept because "unreachable" is a claim about another file.
        return WallClockError::EPOCH_OUT_OF_RANGE;
    }

    wall_clock().store(*offset);
    return {};
}

}  // namespace pico_time
