// SPDX-License-Identifier: MIT
#include "wall_clock.hpp"

#include <cstdint>
#include <optional>
#include <pico_time/core/epoch.hpp>

namespace pico_time {
namespace {

constexpr std::int64_t MICROSECONDS_PER_SECOND = 1'000'000;

}  // namespace

std::optional<std::int64_t> compute_epoch_offset(
    std::uint64_t epoch_seconds, std::uint32_t epoch_microseconds,
    std::int64_t monotonic_us) {
    if (epoch_microseconds > MAX_MICROSECONDS) {
        return std::nullopt;
    }
    // The seconds bound also keeps the multiply below inside int64_t.
    if (epoch_seconds < MIN_EPOCH_SECONDS ||
        epoch_seconds >= MAX_EPOCH_SECONDS) {
        return std::nullopt;
    }

    const std::int64_t desired =
        (static_cast<std::int64_t>(epoch_seconds) * MICROSECONDS_PER_SECOND) +
        static_cast<std::int64_t>(epoch_microseconds);
    return desired - monotonic_us;
}

std::int64_t apply_offset(std::int64_t monotonic_us, std::int64_t offset_us) {
    const std::int64_t adjusted = monotonic_us + offset_us;
    return adjusted < 0 ? 0 : adjusted;
}

WallClock& wall_clock() {
    // Constant initialisation avoids a first-call guard race with interrupt
    // reads.
    static constinit WallClock clock;
    return clock;
}

}  // namespace pico_time
