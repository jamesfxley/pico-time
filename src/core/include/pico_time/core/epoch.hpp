// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

/// \file epoch.hpp
/// The wall-clock plausibility window.

namespace pico_time {

/// Earliest accepted Unix epoch second: 2025-01-01T00:00:00Z.
inline constexpr std::uint64_t MIN_EPOCH_SECONDS = 1735689600ULL;

/// Accepted window above the floor, in seconds: ten Julian years (FR-8, NR-5).
inline constexpr std::uint64_t EPOCH_WINDOW_SECONDS = 315576000ULL;

/// Exclusive upper bound in Unix epoch seconds: 2035-01-01T12:00:00Z.
inline constexpr std::uint64_t MAX_EPOCH_SECONDS =
    MIN_EPOCH_SECONDS + EPOCH_WINDOW_SECONDS;

}  // namespace pico_time
