// SPDX-License-Identifier: MIT
#pragma once

/// \file pico_time.hpp
/// pico-time umbrella header. Pulls in the full public surface, so a consumer
/// can include this alone and synchronize the wall clock.

// The umbrella re-exports the public surface.
#include <pico_time/time_sync/time_sync.hpp>  // IWYU pragma: export

namespace pico_time {

/// Semantic version of the library, mirroring project(pico_time VERSION ...) in
/// the root CMakeLists.txt. Pinned from both sides:
/// tests/core/pico_time_test.cpp checks these on the host, and time_keeper.cpp
/// static_asserts them against the version CMake passes in, so the two cannot
/// drift apart. Major version. Changes when a documented interface breaks.
inline constexpr int VERSION_MAJOR = 0;
/// Minor version. Changes when the interface grows compatibly.
inline constexpr int VERSION_MINOR = 1;
/// Patch version. Changes when behaviour is corrected within the interface.
inline constexpr int VERSION_PATCH = 0;

}  // namespace pico_time
