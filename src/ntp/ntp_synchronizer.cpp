// SPDX-License-Identifier: MIT
// The attempt loop and the slot pool. See ntp_synchronizer.hpp for why this is
// above the transport seam rather than inside the lwIP adapter.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <pico_time/ntp/ntp_error.hpp>
#include <pico_time/ntp/ntp_synchronizer.hpp>
#include <pico_time/ntp/ntp_transport.hpp>
#include <pico_util/result/result.hpp>
#include <span>

#include "ntp_message.hpp"

namespace pico_time {
namespace {

/// Map a wire verdict onto the public taxonomy.
///
/// ORIGINATE_MISMATCH has no mapping on purpose: a datagram that has not proved
/// it belongs to this exchange is dropped rather than allowed to end an attempt
/// (VR-7), so it never reaches here.
[[nodiscard]] NtpError from_reject(NtpReject reason) {
    switch (reason) {
        case NtpReject::MODE:
            return NtpError::REJECTED_MODE;
        case NtpReject::LEAP_ALARM:
            return NtpError::REJECTED_LEAP_ALARM;
        case NtpReject::STRATUM:
            return NtpError::REJECTED_STRATUM;
        case NtpReject::TIMESTAMP_ZERO:
            return NtpError::REJECTED_TIMESTAMP_ZERO;
        case NtpReject::TIMESTAMP_IMPLAUSIBLE:
            return NtpError::REJECTED_TIMESTAMP_IMPLAUSIBLE;
        case NtpReject::ORIGINATE_MISMATCH:
        case NtpReject::NONE:
            break;
    }
    return NtpError::TIMED_OUT;
}

}  // namespace

void NtpSynchronizer::Slot::on_resolved(const NtpAddress& address) {
    // Runs in the transport's callback context, holding its lock. Clear the
    // pending flag first, then check whether the caller gave up: if so it has
    // already torn the attempt down and this slot is ours to release.
    resolve_pending = false;
    if (abandoned) {
        in_use = false;
        return;
    }
    if (!address.valid) {
        error = NtpError::DNS_FAILED;
        complete = true;
        return;
    }
    // Record and return. The request is sent by the mainline once it observes
    // this flag, so nothing is allocated and no lock is taken here (VR-8).
    server_address = address;
    address_ready = true;
}

void NtpSynchronizer::Slot::on_datagram(
    const NtpAddress& sender, std::span<const std::uint8_t> datagram) {
    // A datagram from somewhere else, or too short to be an NTP message, is not
    // this exchange's failure. Drop it and keep waiting rather than letting a
    // stray packet end the attempt.
    if (!address_ready || sender.opaque != server_address.opaque ||
        datagram.size() < NTP_MESSAGE_LENGTH) {
        return;
    }

    std::array<std::uint8_t, NTP_MESSAGE_LENGTH> raw{};
    std::copy_n(datagram.begin(), NTP_MESSAGE_LENGTH, raw.begin());

    const NtpParse parse = parse_ntp_response(raw, nonce);
    if (!parse.sample.has_value()) {
        // VR-7. The nonce check runs first, so ORIGINATE_MISMATCH is the only
        // verdict a datagram that has not proved it belongs to this exchange
        // can reach. Dropping it -- rather than recording a failure -- is what
        // stops a forgery or a replay from ending an attempt that would
        // otherwise have succeeded. Every other verdict is a real server's
        // report of itself, and does end the attempt.
        if (parse.reason == NtpReject::ORIGINATE_MISMATCH) {
            return;
        }
        error = from_reject(parse.reason);
        complete = true;
        return;
    }

    sample = SynchronizedTime{
        .epoch_seconds = parse.sample->epoch_seconds,
        .epoch_microseconds = parse.sample->epoch_microseconds};
    error = NtpError::NONE;
    complete = true;
}

void NtpSynchronizer::Slot::reset() {
    server_address = NtpAddress{};
    nonce = 0;
    address_ready = false;
    complete = false;
    error = NtpError::NONE;
    sample = SynchronizedTime{};
    resolve_pending = false;
    abandoned = false;
    in_use = false;
}

NtpSynchronizer::Slot* NtpSynchronizer::claim_slot() {
    // Call with the transport's lock held: it is what serializes this against
    // the callback that releases a slot.
    for (Slot& slot : slots_) {
        if (!slot.in_use) {
            slot.reset();
            slot.owner = this;
            slot.in_use = true;
            return &slot;
        }
    }
    return nullptr;
}

void NtpSynchronizer::release_slot(Slot& slot) {
    // Give the slot back, or mark it for the resolution callback to give back
    // when the lookup it cannot cancel finally lands. Lock held.
    if (slot.resolve_pending) {
        slot.abandoned = true;
        return;
    }
    slot.in_use = false;
}

std::size_t NtpSynchronizer::slots_in_use() const {
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.in_use) {
            ++count;
        }
    }
    return count;
}

bool NtpSynchronizer::poll_until(const Slot& slot, std::uint64_t start_ms,
                                 std::uint64_t budget_ms,
                                 bool (*ready)(const Slot&)) {
    while (!ready(slot)) {
        // Unsigned difference, so a counter that steps back ends the wait
        // early instead of never ending it. An absolute deadline cannot: a
        // clock that wraps past it leaves a comparison that is false forever,
        // which on the no-reply path is a mainline hang rather than a timeout.
        if (transport_->now_ms() - start_ms >= budget_ms) {
            return false;
        }
        transport_->poll();
        transport_->wait(POLL_DELAY);
    }
    return true;
}

NtpError NtpSynchronizer::run_attempt(Slot& slot,
                                      std::chrono::milliseconds timeout) {
    // One budget for the whole attempt, so resolution and reply share the
    // caller's allowance instead of each being given a full copy of it.
    const std::uint64_t start_ms = transport_->now_ms();
    const auto budget_ms = static_cast<std::uint64_t>(timeout.count());

    if (!transport_->open(slot)) {
        return NtpError::SOCKET_FAILED;
    }

    // Start the lookup under the lock, so the callback -- which needs the same
    // lock -- cannot fire and clear resolve_pending before it is set.
    transport_->lock();
    const ResolveStatus status =
        transport_->resolve(server_, slot.server_address, slot);
    if (status == ResolveStatus::IN_PROGRESS) {
        slot.resolve_pending = true;
    } else if (status == ResolveStatus::RESOLVED) {
        slot.address_ready = true;
    }
    transport_->unlock();

    if (status == ResolveStatus::FAILED) {
        transport_->close();
        return NtpError::DNS_FAILED;
    }

    if (!poll_until(slot, start_ms, budget_ms, [](const Slot& state) {
            return state.address_ready || state.complete;
        })) {
        transport_->close();
        return NtpError::TIMED_OUT;
    }
    if (slot.complete) {
        transport_->close();
        return slot.error;
    }

    const std::array<std::uint8_t, NTP_MESSAGE_LENGTH> request =
        build_ntp_request(slot.nonce);
    if (!transport_->send(slot.server_address, request)) {
        transport_->close();
        return NtpError::SEND_FAILED;
    }

    const bool completed =
        poll_until(slot, start_ms, budget_ms,
                   [](const Slot& state) { return state.complete; });

    // Close before reading `sample`. A second datagram could otherwise
    // overwrite the timestamp halves while the caller reads them (CXX-08.1).
    transport_->close();

    if (!completed) {
        return NtpError::TIMED_OUT;
    }
    return slot.error;
}

pico_util::Result<SynchronizedTime, NtpError> NtpSynchronizer::synchronize(
    std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        return NtpError::INVALID_TIMEOUT;
    }

    NtpError last = NtpError::TIMED_OUT;
    // Fresh state, nonce and endpoint per attempt: a late response to an
    // abandoned attempt must not land in the next attempt's callbacks, and a
    // reused nonce would make it indistinguishable from a fresh reply.
    for (std::size_t attempt = 0; attempt < MAX_REQUEST_ATTEMPTS; ++attempt) {
        transport_->lock();
        Slot* const slot = claim_slot();
        transport_->unlock();
        if (slot == nullptr) {
            // AR-6. Every slot is held by a lookup that has not landed, which
            // takes a previous call whose attempts all timed out inside
            // resolution. Transient: every lookup calls back, and that callback
            // releases the slot. Returned rather than retried, because nothing
            // this loop does can clear it.
            //
            // Only from the first attempt, which is what AR-6 promises: no
            // network traffic and no attempt spent. Reaching here later means
            // this call has already opened an endpoint and issued a lookup, so
            // reporting an empty pool would both contradict that promise and
            // discard the diagnosis those attempts produced.
            if (attempt == 0) {
                return NtpError::NO_ATTEMPT_SLOT;
            }
            return last;
        }
        slot->nonce = transport_->nonce();

        const NtpError error = run_attempt(*slot, timeout);
        const SynchronizedTime sample = slot->sample;

        // Decide the slot's fate under the lock, serialized against the
        // resolution callback.
        transport_->lock();
        release_slot(*slot);
        transport_->unlock();

        if (error == NtpError::NONE) {
            return sample;
        }
        last = error;

        // No point waiting after the final attempt.
        if (attempt + 1 < MAX_REQUEST_ATTEMPTS) {
            transport_->wait(RETRY_DELAY);
        }
    }
    return last;
}

const char* describe(NtpError error) {
    switch (error) {
        case NtpError::NONE:
            return "none";
        case NtpError::INVALID_TIMEOUT:
            return "invalid timeout";
        case NtpError::NO_ATTEMPT_SLOT:
            return "no free attempt slot";
        case NtpError::DNS_FAILED:
            return "DNS lookup failed";
        case NtpError::SOCKET_FAILED:
            return "UDP endpoint unavailable";
        case NtpError::SEND_FAILED:
            return "send failed";
        case NtpError::TIMED_OUT:
            return "timed out";
        case NtpError::REJECTED_MODE:
            return "rejected: not a server reply";
        case NtpError::REJECTED_LEAP_ALARM:
            return "rejected: leap alarm";
        case NtpError::REJECTED_STRATUM:
            return "rejected: stratum";
        case NtpError::REJECTED_TIMESTAMP_ZERO:
            return "rejected: timestamp unset";
        case NtpError::REJECTED_TIMESTAMP_IMPLAUSIBLE:
            return "rejected: timestamp implausible";
        case NtpError::CLOCK_REFUSED:
            return "clock refused the time";
    }
    return "unknown";
}

const char* describe(WallClockError error) {
    switch (error) {
        case WallClockError::NONE:
            return "none";
        case WallClockError::MICROSECONDS_INVALID:
            return "microseconds above 999999";
        case WallClockError::EPOCH_OUT_OF_RANGE:
            return "epoch seconds out of range";
    }
    return "unknown";
}

}  // namespace pico_time
