// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

/// \file epoch.hpp
/// The wall-clock plausibility window.
///
/// These bounds belong to the portable core rather than to synchronization:
/// they are what the SNTP parser and the wall-clock arithmetic validate
/// against, and both are host-tested without any network or SDK. A value
/// outside this window is refused rather than applied, whichever direction it
/// arrived from.
///
/// Requirement IDs cited here are defined in docs/technical-specification.md.

namespace pico_time {

/// Earliest wall-clock time this library will accept, as a Unix epoch second:
/// 2025-01-01T00:00:00Z. Firmware cannot legitimately predate its own release,
/// so an earlier value is an unset clock or a forgery rather than a time.
inline constexpr std::uint64_t MIN_EPOCH_SECONDS = 1735689600ULL;

/// Span of wall-clock time accepted above the floor: ten Julian years. Two
/// things rest on this width (FR-8). It bounds how far a forged or
/// malfunctioning server can move the clock, so a window wide enough to be no
/// constraint is no check. And being narrower than an NTP era, it is what lets
/// the parser decide which era a timestamp belongs to without already knowing
/// the time (NR-5).
inline constexpr std::uint64_t EPOCH_WINDOW_SECONDS = 315576000ULL;

/// Latest wall-clock time this library will accept: 2035-01-01T12:00:00Z for
/// the floor above. A build whose floor is this one refuses every NTP reply
/// from that instant on; moving the window forward is a release's job.
inline constexpr std::uint64_t MAX_EPOCH_SECONDS =
    MIN_EPOCH_SECONDS + EPOCH_WINDOW_SECONDS;

}  // namespace pico_time
