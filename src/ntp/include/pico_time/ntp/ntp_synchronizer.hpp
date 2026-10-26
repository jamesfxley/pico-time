// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <pico_time/ntp/ntp_error.hpp>
#include <pico_time/ntp/ntp_transport.hpp>
#include <pico_util/result/result.hpp>
#include <span>

/// \file ntp_synchronizer.hpp
/// The part of NTP synchronization that is policy rather than plumbing.
///
/// The attempt loop, the slot pool an uncancellable lookup outlives, and the
/// decision about what a failed attempt means. No lwIP, no SDK: everything here
/// reaches the network through INtpTransport, which is what lets AR-6's slot
/// exhaustion and the abandoned-slot release protocol be driven by a test
/// rather than reasoned about.
///
/// Requirement IDs cited here are defined in docs/technical-specification.md.

namespace pico_time {

/// Attempts made before giving up (NR-3).
inline constexpr std::size_t MAX_REQUEST_ATTEMPTS = 3;

/// Delay between attempts. Long enough that a momentarily congested link is not
/// hit three times in the same instant.
inline constexpr auto RETRY_DELAY = std::chrono::milliseconds(1000);

/// How long the poll loop sleeps between servicing the background context.
inline constexpr auto POLL_DELAY = std::chrono::milliseconds(50);

/// What an accepted reply carried, for the caller that applies it.
struct SynchronizedTime {
    /// Seconds since the Unix epoch, inside the plausibility window (VR-6).
    std::uint64_t epoch_seconds = 0;
    /// Microseconds within that second; never above 999999.
    std::uint32_t epoch_microseconds = 0;
};

/// Runs the attempt loop over a transport.
///
/// Statically sized: the slot pool is the only mutable state and it is an array
/// of MAX_REQUEST_ATTEMPTS, which is the most an outstanding lookup can hold --
/// every attempt either releases its own slot or hands the release to the one
/// callback still to fire. Nothing here allocates (VR-8).
class NtpSynchronizer {
   public:
    /// \param transport the environment; must outlive this object.
    /// \param server a NUL-terminated server name, used for every attempt.
    NtpSynchronizer(INtpTransport& transport, const char* server)
        : transport_(&transport), server_(server) {}

    /// Run up to MAX_REQUEST_ATTEMPTS exchanges, each bounded by `timeout`.
    ///
    /// \return the accepted time, or why every attempt failed. The error is the
    ///         last attempt's, except that NO_ATTEMPT_SLOT returns immediately:
    ///         it is a condition of the pool rather than of the network, and
    ///         retrying inside this call cannot clear it (AR-6).
    [[nodiscard]] pico_util::Result<SynchronizedTime, NtpError> synchronize(
        std::chrono::milliseconds timeout);

    /// Slots currently held, including those abandoned to a lookup that has not
    /// landed. Observability, and what a test asserts the release protocol on.
    [[nodiscard]] std::size_t slots_in_use() const;

   private:
    /// One attempt's state, and the observer both callbacks reach it through.
    ///
    /// A lookup cannot be cancelled, so it fires after the attempt that started
    /// it has been torn down. `abandoned` records that, and whichever of the
    /// caller and the callback runs last releases the slot.
    ///
    /// These are plain members rather than atomics, and the argument for that
    /// is the transport's execution model rather than anything visible here.
    /// The writer is lwIP's callback on a single core, under the NO_SYS
    /// configuration `lwip_ntp_transport.cpp` asserts: a callback runs to
    /// completion with respect to the mainline, so there is no interleaving
    /// for an ordering to protect. The mainline's spin on `complete` and
    /// `address_ready` cannot be hoisted either, because every call in the
    /// loop body goes through INtpTransport and out of this translation unit.
    ///
    /// Both halves of that are load-bearing. A preemptive arrangement -- lwIP
    /// on a second core, or a NO_SYS=0 build -- breaks it with no diagnostic
    /// and no failing test, which is why the assert refusing that
    /// configuration is a correctness guard and not a portability note. Under
    /// one, publish these through pico_util::Published, as WallClock does.
    class Slot final : public IResolveObserver, public IDatagramObserver {
       public:
        void on_resolved(const NtpAddress& address) override;
        void on_datagram(const NtpAddress& sender,
                         std::span<const std::uint8_t> datagram) override;

        /// Put this slot back into its unclaimed state.
        ///
        /// Field by field rather than `*this = Slot{}`: the observer bases are
        /// non-copyable, as an interface should be, so a slot cannot be
        /// assigned over.
        void reset();

        /// The synchronizer owning this slot, for the lock and the nonce.
        NtpSynchronizer* owner = nullptr;
        NtpAddress server_address{};
        std::uint64_t nonce = 0;
        /// Set by on_resolved once an address is in place.
        bool address_ready = false;
        /// Set when an attempt has finished, successfully or not.
        bool complete = false;
        /// Why it finished, NONE while it has not.
        NtpError error = NtpError::NONE;
        SynchronizedTime sample{};
        /// A resolution is outstanding and will call back.
        bool resolve_pending = false;
        /// The caller gave up; the callback releases this slot.
        bool abandoned = false;
        /// Claimed by an attempt.
        bool in_use = false;
    };

    [[nodiscard]] Slot* claim_slot();
    static void release_slot(Slot& slot);
    [[nodiscard]] NtpError run_attempt(Slot& slot,
                                       std::chrono::milliseconds timeout);
    /// Poll until `ready`, or until `budget_ms` has elapsed since `start_ms`.
    ///
    /// Elapsed difference rather than an absolute deadline: `start_ms +
    /// budget_ms` can land past the end of a transport's counter, and a
    /// deadline the counter never reaches is a wait that never ends.
    [[nodiscard]] bool poll_until(const Slot& slot, std::uint64_t start_ms,
                                  std::uint64_t budget_ms,
                                  bool (*ready)(const Slot&));

    INtpTransport* transport_;
    const char* server_;
    std::array<Slot, MAX_REQUEST_ATTEMPTS> slots_{};
};

}  // namespace pico_time
