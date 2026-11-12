// SPDX-License-Identifier: MIT
#pragma once

/// \file pico_time.hpp
/// The firmware API and version constants.

// The umbrella re-exports the public surface.
#include <pico_time/time_sync/time_sync.hpp>  // IWYU pragma: export

namespace pico_time {

/// Major version.
inline constexpr int VERSION_MAJOR = 0;
/// Minor version.
inline constexpr int VERSION_MINOR = 2;
/// Patch version.
inline constexpr int VERSION_PATCH = 0;

}  // namespace pico_time
