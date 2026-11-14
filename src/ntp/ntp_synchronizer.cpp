// SPDX-License-Identifier: MIT

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

/// Map a wire verdict to a public error. Nonce mismatches never arrive (VR-7).
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
    // Clear pending before checking abandonment. The callback owns release
    // once the caller has abandoned the slot, with the transport lock held.
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
    // The mainline sends after observing address_ready. No allocation or lock
    // acquisition occurs in this callback (VR-8).
    server_address = address;
    address_ready = true;
}

void NtpSynchronizer::Slot::on_datagram(
    const NtpAddress& sender, std::span<const std::uint8_t> datagram) {
    // Stray or short datagrams leave the attempt waiting for its reply.
    if (!address_ready || sender.opaque != server_address.opaque ||
        datagram.size() < NTP_MESSAGE_LENGTH) {
        return;
    }

    std::array<std::uint8_t, NTP_MESSAGE_LENGTH> raw{};
    std::copy_n(datagram.begin(), NTP_MESSAGE_LENGTH, raw.begin());

    const NtpParse parse = parse_ntp_response(raw, nonce);
    if (!parse.sample.has_value()) {
        // A nonce mismatch leaves the attempt waiting. Other rejections end it
        // (VR-7).
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
    // Call with the transport lock held to serialise callback releases.
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
    // With the lock held, defer release to any outstanding resolution callback.
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
        // The loop uses an elapsed unsigned difference, not an absolute
        // deadline. A deadline past a wrapping counter is never reached, and
        // the no-reply wait would never end. A backward clock step instead ends
        // the wait early.
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
    // Resolution and reply share one timeout.
    const std::uint64_t start_ms = transport_->now_ms();
    const auto budget_ms = static_cast<std::uint64_t>(timeout.count());

    if (!transport_->open(slot)) {
        return NtpError::SOCKET_FAILED;
    }

    // The lookup starts under the lock. Its callback needs the same lock, so
    // it cannot clear resolve_pending before it is set.
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

    // Close before reading sample to prevent another datagram overwriting it.
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
    // A late reply must not reach the next attempt. Each has a fresh nonce
    // and endpoint.
    for (std::size_t attempt = 0; attempt < MAX_REQUEST_ATTEMPTS; ++attempt) {
        transport_->lock();
        Slot* const slot = claim_slot();
        transport_->unlock();
        if (slot == nullptr) {
            // Slot exhaustion is reported only on entry, before network traffic
            // (AR-6). Later exhaustion returns the preceding attempt's error.
            if (attempt == 0) {
                return NtpError::NO_ATTEMPT_SLOT;
            }
            return last;
        }
        slot->nonce = transport_->nonce();

        const NtpError error = run_attempt(*slot, timeout);
        const SynchronizedTime sample = slot->sample;

        // Serialise release with the resolution callback.
        transport_->lock();
        release_slot(*slot);
        transport_->unlock();

        if (error == NtpError::NONE) {
            return sample;
        }
        last = error;

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
