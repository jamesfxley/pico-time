// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "pico_util/publish/published.hpp"
#include "pico_util/result/result.hpp"

/// \file wall_clock.hpp
/// The wall clock: a signed offset from a monotonic counter, and the
/// publication protocol that lets an interrupt handler read it safely.
///
/// Free of lwIP and of the Pico SDK so the arithmetic and the publication
/// protocol are both host-testable; the SDK's monotonic counter is supplied by
/// the caller rather than read here. Requirement IDs are defined in
/// docs/technical-specification.md.

namespace pico_time {

/// Largest legal sub-second component of a wall-clock time.
inline constexpr std::uint32_t MAX_MICROSECONDS = 999999U;

/// Offset to add to a monotonic reading to obtain epoch microseconds, or
/// nullopt if the arguments are not a time this library will represent.
///
/// Rejecting is the point (FR-5): epoch_seconds is bounded so the multiply
/// cannot overflow int64 (CXX-12.4), which also catches the caller who passes
/// microseconds-since-epoch where seconds were wanted -- a value three orders
/// of magnitude past anything valid.
[[nodiscard]] std::optional<std::int64_t> compute_epoch_offset(
    std::uint64_t epoch_seconds, std::uint32_t epoch_microseconds,
    std::int64_t monotonic_us);

/// Apply a stored offset to a monotonic reading, clamped non-negative.
///
/// The clamp is defence, not correction: compute_epoch_offset() refuses every
/// input that could produce a negative result, so this cannot fire without a
/// defect elsewhere (FR-3).
[[nodiscard]] std::int64_t apply_offset(std::int64_t monotonic_us,
                                        std::int64_t offset_us);

/// The publication protocol behind WallClock, named once so the class body and
/// its SLOT_COUNT both read from one declaration.
using Offset = pico_util::Published<
    std::int64_t, pico_util::PublishStrategy::SLOT_RING_READER_MAY_PREEMPT>;

/// Wall-clock offset shared between mainline code and interrupt context.
///
/// The offset is 64 bits and this is a 32-bit machine, so it needs a
/// publication protocol (CXX-08.4). A ring of buffers rather than a seqlock:
/// a seqlock reader spins until the writer finishes, and an interrupt-context
/// reader that has pre-empted its own mainline writer would spin forever. Here
/// the reader never spins -- it validates and retries a bounded number of
/// times. That is `pico_util::Published`'s SLOT_RING_READER_MAY_PREEMPT
/// strategy, named rather than defaulted because the opposite choice is
/// correct for the opposite preemption direction and neither is distinguishable
/// in a host test.
///
/// The ring is what makes a validated read safe rather than merely likely. A
/// reader's slot is only ever disturbed when the writer comes all the way back
/// round to it, which takes SLOT_COUNT stores; the writer is already inside
/// the next slot one store before it publishes, so a read tolerates
/// SLOT_COUNT - 2 stores landing underneath it and no more.
///
/// Every atomic the strategy uses is 32 bits wide, which is what makes this
/// genuinely lock-free on this target. A std::atomic<std::int64_t> would
/// instead call __atomic_load_8, which the SDK implements as a global spinlock
/// with interrupts disabled.
///
/// WRITER CONTRACT: one writer, mainline context only, and at most one store
/// per second. It is what bounds the reader: disturbing a read in flight takes
/// SLOT_COUNT - 1 stores inside the handful of instructions a read occupies,
/// and exhausting the retries takes that many during each of them.
///
/// Only the rate limit holds by construction, and only for synchronize_ntp():
/// it stores once, on success, and once synchronized returns before any work
/// (NR-4), so it stores at most once per boot. The other public writers --
/// apply_wall_clock_time() and the settimeofday() shim over it -- reach store()
/// with no rate limit and from whatever context their caller runs in, so for
/// them the contract is an obligation stated on those entry points rather than
/// a property of this code. Nothing enforces it.
class WallClock {
   public:
    /// Slots the publication ring holds, and the bound FR-6 is stated against.
    static constexpr std::size_t SLOT_COUNT = Offset::SLOT_COUNT;

    /// Publish a new offset and latch the clock as synchronized.
    void store(std::int64_t offset_us) { offset_.store(offset_us); }

    /// Read the current offset. Safe from any context; never blocks.
    ///
    /// \return the offset, or `PublishedReadError::EXHAUSTED` when the read
    ///         could not be validated. Unreachable under the writer contract
    ///         above; a writer that breaks it -- apply_wall_clock_time() called
    ///         in a tight loop -- is reported here rather than handed back as a
    ///         possibly torn offset.
    [[nodiscard]] pico_util::Result<std::int64_t, pico_util::PublishedReadError>
    load() const {
        return offset_.load();
    }

    /// True once an offset has been published. Never becomes false again.
    ///
    /// Ordered after the offset by the strategy, so a reader that observes the
    /// clock as synchronized sees the offset that made it so.
    [[nodiscard]] bool synchronized() const { return offset_.published(); }

   private:
    Offset offset_;
};

/// The process-wide wall clock behind the newlib syscall shims.
WallClock& wall_clock();

}  // namespace pico_time
