// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "pico_util/publish/published.hpp"
#include "pico_util/result/result.hpp"

/// \file wall_clock.hpp
/// A wall-clock offset supplied with a monotonic counter by the caller.

namespace pico_time {

/// Largest legal sub-second component of a wall-clock time.
inline constexpr std::uint32_t MAX_MICROSECONDS = 999999U;

/// Epoch offset in microseconds, or nullopt for invalid time (FR-5).
[[nodiscard]] std::optional<std::int64_t> compute_epoch_offset(
    std::uint64_t epoch_seconds, std::uint32_t epoch_microseconds,
    std::int64_t monotonic_us);

/// Add an offset to a monotonic reading and clamp at zero, in microseconds
/// (FR-3).
[[nodiscard]] std::int64_t apply_offset(std::int64_t monotonic_us,
                                        std::int64_t offset_us);

/// Publication type for WallClock, named once for its member and SLOT_COUNT.
using Offset = pico_util::Published<
    std::int64_t, pico_util::PublishStrategy::SLOT_RING_READER_MAY_PREEMPT>;

/// An offset shared with interrupt readers through a slot ring.
/// WRITER CONTRACT: one mainline writer makes at most one store per second.
/// synchronize_ntp() stores at most once per boot (NR-4).
/// Direct setters do not enforce this contract.
class WallClock {
   public:
    /// Number of publication slots (FR-6).
    static constexpr std::size_t SLOT_COUNT = Offset::SLOT_COUNT;

    /// Publish a new offset and latch the clock as synchronised.
    void store(std::int64_t offset_us) { offset_.store(offset_us); }

    /// Read an offset from any context without blocking.
    /// \return the offset, or EXHAUSTED if validation fails.
    [[nodiscard]] pico_util::Result<std::int64_t, pico_util::PublishedReadError>
    load() const {
        return offset_.load();
    }

    /// True after the first store, with that offset visible. Never becomes
    /// false.
    [[nodiscard]] bool synchronized() const { return offset_.published(); }

   private:
    Offset offset_;
};

/// The process-wide wall clock behind the newlib syscall shims.
WallClock& wall_clock();

}  // namespace pico_time
