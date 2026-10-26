// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <pico_time/ntp/ntp_transport.hpp>
#include <span>
#include <utility>
#include <vector>

/// \file fake_ntp_transport.hpp
/// A scripted INtpTransport, for testing an NTP policy without a network.
///
/// Shipped rather than kept under tests/: a downstream project driving
/// NtpSynchronizer in its own tests needs the same double this repository's
/// tests use, and a copy in each tree is a copy free to drift from the contract
/// it stands in for (EN-04.1).
///
/// The contract this fake honours, and the reason it exists:
///
///   * `resolve()` returning IN_PROGRESS **cannot be cancelled**. The callback
///     fires exactly once, whenever the script says, including after the
///     attempt that started it has been torn down. That is the case the slot
///     pool exists for, and the one no run on hardware can induce.
///   * A callback runs **only while the lock is not held**, which is what the
///     real transport's cyw43_arch_lwip_begin/end guarantees and what the slot
///     protocol's correctness rests on. A resolution delivered under the lock
///     is queued and fires at unlock, exactly as lwIP would.
///   * A datagram arrives **from inside the poll loop**, which is a
///     simplification and not what the target does. This library links
///     pico_cyw43_arch_lwip_threadsafe_background, whose async context has no
///     poll function, so poll() is a no-op on target and lwIP delivers from a
///     background IRQ -- in practice during the sleep in wait(), and in
///     principle at any instruction. Arrivals are scripted by poll index
///     because that is what a synchronous fake can express; it is a subset of
///     the target's orders, not the whole of them, and no test written here
///     can place a callback inside the policy's own poll loop.

namespace pico_time::testing {

/// A transport whose every effect is scripted by the test that owns it.
class FakeNtpTransport final : public INtpTransport {
   public:
    FakeNtpTransport() = default;

    /// A test double is constructed on the stack of the test that owns it and
    /// is never copied: it is registered with the policy by reference, so a
    /// copy would be scripted and observed independently of the one in use.
    FakeNtpTransport(const FakeNtpTransport&) = delete;
    FakeNtpTransport& operator=(const FakeNtpTransport&) = delete;
    FakeNtpTransport(FakeNtpTransport&&) = delete;
    FakeNtpTransport& operator=(FakeNtpTransport&&) = delete;
    ~FakeNtpTransport() = default;

    /// An address the scripted resolution reports. Any distinct tag will do:
    /// the policy does not interpret one, it only compares sender against
    /// server, so two tags differ exactly when two endpoints do.
    [[nodiscard]] static NtpAddress address(std::uint8_t tag) {
        NtpAddress out;
        out.opaque[0] = tag;
        out.valid = true;
        return out;
    }

    // -- Script ------------------------------------------------------------

    /// \param status what every subsequent resolve() returns.
    void set_resolve_result(ResolveStatus status) { resolve_result_ = status; }

    /// \param ok what every subsequent open() returns.
    void set_open_result(bool ok) { open_result_ = ok; }

    /// \param ok what every subsequent send() returns.
    void set_send_result(bool ok) { send_result_ = ok; }

    /// Land the lookup inside resolve(), while the policy still holds the
    /// lock.
    ///
    /// The one interleaving the slot protocol is built for, and the only way
    /// to reach the queue: every other delivery point in this fake runs at
    /// depth zero. lwIP can call back from inside the call that starts the
    /// lookup, and the policy's correctness rests on that callback being
    /// serialized behind the unlock rather than running inside it.
    ///
    /// \param resolved the address that resolution reports.
    void resolve_lands_under_the_lock(const NtpAddress& resolved) {
        resolve_inline_ = true;
        resolve_address_ = resolved;
    }

    /// \param at_poll the poll index an outstanding resolution lands on,
    ///        counting from one; zero leaves it outstanding forever, which is
    ///        the state the slot pool exists to survive.
    /// \param resolved the address that resolution reports.
    void resolve_on_poll(std::size_t at_poll, const NtpAddress& resolved) {
        resolve_at_poll_ = at_poll;
        resolve_address_ = resolved;
    }

    /// Deliver `datagram` from `sender` on poll number `at_poll`.
    void arrive_on_poll(std::size_t at_poll, const NtpAddress& sender,
                        std::vector<std::uint8_t> datagram) {
        arrivals_.push_back({at_poll, sender, std::move(datagram)});
    }

    /// \param value the next nonce; it increments after each nonce() call, so
    ///        two attempts never share one, as the real source does not.
    void set_next_nonce(std::uint64_t value) { next_nonce_ = value; }

    /// \param value the instant now_ms() reports until the next wait().
    void set_now(std::uint64_t value) { now_ = value; }

    /// Report time from a 32-bit counter rather than the 64-bit one the seam
    /// declares.
    ///
    /// This is the width the SDK's to_ms_since_boot() actually has, so a fake
    /// that only ever counts up in 64 bits is a more capable clock than the
    /// thing it stands in for -- and no test written against it can reach the
    /// wrap a real attempt has to survive (CXX-08.6).
    ///
    /// \param narrow whether now_ms() truncates to 32 bits.
    void set_narrow_clock(bool narrow) { narrow_clock_ = narrow; }

    // -- Observation -------------------------------------------------------

    /// \return the nonce the next nonce() call will hand out.
    [[nodiscard]] std::uint64_t next_nonce() const { return next_nonce_; }

    /// \return resolution requests made.
    [[nodiscard]] std::size_t resolve_calls() const { return resolve_calls_; }

    /// \return endpoints opened; one per attempt, so this counts attempts.
    [[nodiscard]] std::size_t open_calls() const { return open_calls_; }

    /// \return endpoints closed; equals open_calls() once a call has returned.
    [[nodiscard]] std::size_t close_calls() const { return close_calls_; }

    /// \return requests put on the wire.
    [[nodiscard]] std::size_t send_calls() const { return send_calls_; }

    /// \return poll-loop iterations; arrivals are scripted against this.
    [[nodiscard]] std::size_t poll_calls() const { return poll_calls_; }

    /// \return every wait() duration in order: poll delays and retry delays,
    ///         truncated at MAX_RECORDED_WAITS.
    [[nodiscard]] const std::vector<std::chrono::milliseconds>& waits() const {
        return waits_;
    }

    /// \return the request bytes the policy last put on the wire.
    [[nodiscard]] std::span<const std::uint8_t> last_sent() const {
        return last_sent_;
    }

    /// \return whether a resolution is still outstanding.
    [[nodiscard]] bool resolution_outstanding() const {
        return !observers_.empty();
    }

    /// \return whether the endpoint is open.
    [[nodiscard]] bool is_open() const { return receiver_ != nullptr; }

    // -- Driving -----------------------------------------------------------

    /// Fire the oldest outstanding resolution callback.
    ///
    /// Under the lock this is queued and fires at unlock, which is what lwIP
    /// does; outside it, it fires now. Does nothing when no lookup is
    /// outstanding, so a test can call it unconditionally.
    void deliver_resolution(const NtpAddress& resolved) {
        if (observers_.empty()) {
            return;
        }
        IResolveObserver* const observer = observers_.front();
        observers_.erase(observers_.begin());
        if (depth_ > 0) {
            pending_.push_back({observer, resolved});
            return;
        }
        observer->on_resolved(resolved);
    }

    /// Advance the monotonic clock without going through wait().
    void advance(std::chrono::milliseconds duration) {
        now_ += static_cast<std::uint64_t>(duration.count());
    }

    // -- INtpTransport -----------------------------------------------------

    void lock() override { ++depth_; }

    void unlock() override {
        --depth_;
        if (depth_ == 0) {
            drain_pending();
        }
    }

    [[nodiscard]] ResolveStatus resolve(const char* /*host*/, NtpAddress& out,
                                        IResolveObserver& observer) override {
        ++resolve_calls_;
        if (resolve_result_ == ResolveStatus::RESOLVED) {
            out = address(1);
        }
        if (resolve_result_ == ResolveStatus::IN_PROGRESS) {
            observers_.push_back(&observer);
            if (resolve_inline_) {
                // Inside the caller's lock, so this queues rather than firing.
                deliver_resolution(resolve_address_);
            }
        }
        return resolve_result_;
    }

    [[nodiscard]] bool open(IDatagramObserver& observer) override {
        ++open_calls_;
        if (open_result_) {
            receiver_ = &observer;
        }
        return open_result_;
    }

    void close() override {
        ++close_calls_;
        receiver_ = nullptr;
    }

    [[nodiscard]] bool send(const NtpAddress& /*server*/,
                            std::span<const std::uint8_t> payload) override {
        ++send_calls_;
        last_sent_.assign(payload.begin(), payload.end());
        return send_result_;
    }

    void poll() override {
        ++poll_calls_;
        if (resolve_at_poll_ != 0 && poll_calls_ == resolve_at_poll_) {
            deliver_resolution(resolve_address_);
        }
        for (std::size_t i = 0; i < arrivals_.size();) {
            if (arrivals_[i].at_poll != poll_calls_) {
                ++i;
                continue;
            }
            const Arrival arrival = arrivals_[i];
            arrivals_.erase(arrivals_.begin() + static_cast<std::ptrdiff_t>(i));
            if (receiver_ != nullptr) {
                receiver_->on_datagram(arrival.sender, arrival.datagram);
            }
        }
    }

    [[nodiscard]] std::uint64_t now_ms() const override {
        return narrow_clock_ ? (now_ & CLOCK_MASK_32) : now_;
    }

    void wait(std::chrono::milliseconds duration) override {
        // Recorded up to a ceiling. A poll loop that never ends is exactly the
        // policy defect a fake has to survive being on the wrong side of, and
        // an unbounded record turns one into an unbounded allocation -- a dead
        // host rather than a failing test. No terminating attempt comes near
        // the cap, so nothing a passing test asserts is truncated.
        if (waits_.size() < MAX_RECORDED_WAITS) {
            waits_.push_back(duration);
        }
        now_ += static_cast<std::uint64_t>(duration.count());
    }

    [[nodiscard]] std::uint64_t nonce() override { return next_nonce_++; }

   private:
    struct Arrival {
        std::size_t at_poll = 0;
        NtpAddress sender;
        std::vector<std::uint8_t> datagram;
    };

    struct Queued {
        IResolveObserver* observer;
        NtpAddress address;
    };

    void drain_pending() {
        while (!pending_.empty()) {
            const Queued queued = pending_.front();
            pending_.erase(pending_.begin());
            queued.observer->on_resolved(queued.address);
        }
    }

    ResolveStatus resolve_result_ = ResolveStatus::IN_PROGRESS;
    bool open_result_ = true;
    bool send_result_ = true;
    std::uint64_t next_nonce_ = 1;
    std::size_t resolve_at_poll_ = 0;
    bool resolve_inline_ = false;
    NtpAddress resolve_address_ = address(1);
    std::vector<Arrival> arrivals_;
    std::size_t resolve_calls_ = 0;
    std::size_t open_calls_ = 0;
    std::size_t close_calls_ = 0;
    std::size_t send_calls_ = 0;
    std::size_t poll_calls_ = 0;
    std::vector<std::chrono::milliseconds> waits_;
    std::vector<IResolveObserver*> observers_;
    std::vector<Queued> pending_;
    std::vector<std::uint8_t> last_sent_;
    IDatagramObserver* receiver_ = nullptr;
    static constexpr std::uint64_t CLOCK_MASK_32 = 0xFFFFFFFFU;
    static constexpr std::size_t MAX_RECORDED_WAITS = 1024;

    std::uint64_t now_ = 0;
    bool narrow_clock_ = false;
    int depth_ = 0;
};

}  // namespace pico_time::testing
