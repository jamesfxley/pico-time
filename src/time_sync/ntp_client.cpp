// SPDX-License-Identifier: MIT
// synchronize_ntp(): wire the lwIP transport to the portable attempt loop, and
// apply what it returns.
//
// The decisions -- how many attempts, which datagrams to trust, when an
// abandoned lookup's slot may be reused -- belong to
// pico_time::NtpSynchronizer, above the transport seam and host-tested. What is
// here is the composition and the log lines, which need the target.

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <pico_time/ntp/ntp_error.hpp>
#include <pico_time/ntp/ntp_synchronizer.hpp>
#include <pico_time/time_sync/time_sync.hpp>
#include <pico_util/result/result.hpp>

#include "lwip_ntp_transport.hpp"
#include "pico_logging/logging/logging.hpp"

namespace pico_time {
namespace {

// A const char* rather than a string_view: its only use is the lwIP DNS call
// below the seam, which requires NUL termination that a view cannot promise.
constexpr const char* NTP_SERVER = "pool.ntp.org";
constexpr std::size_t TIME_STRING_SIZE = 20;  // "YYYY-MM-DD HH:MM:SS" + NUL

void log_synchronized(const SynchronizedTime& sample) {
    const auto epoch_seconds = static_cast<time_t>(sample.epoch_seconds);

    // A failed conversion leaves utc zeroed, which strftime renders as a
    // plausible 1900 date. The log line is cosmetic -- the clock has already
    // been set -- so say so explicitly rather than print a wrong time.
    //
    // strftime into a buffer rather than streaming std::put_time: LogLine has
    // no inserter for the put_time proxy and would render it as "<?>".
    struct tm utc{};
    std::array<char, TIME_STRING_SIZE> formatted{};
    if (gmtime_r(&epoch_seconds, &utc) == nullptr ||
        std::strftime(formatted.data(), formatted.size(), "%Y-%m-%d %H:%M:%S",
                      &utc) == 0) {
        PICO_LOG_INFO << "NTP: Synchronized to epoch="
                      << static_cast<std::int64_t>(epoch_seconds)
                      << " (unavailable)";
        return;
    }
    PICO_LOG_INFO << "NTP: Synchronized to " << formatted.data() << " UTC";
}

}  // namespace

pico_util::Result<void, NtpError> synchronize_ntp(
    std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0 || timeout > MAX_SYNC_TIMEOUT) {
        return NtpError::INVALID_TIMEOUT;
    }

    if (is_time_synchronized()) {
        return {};
    }

    // Static, not a local: the slot pool inside it must outlive this call. A
    // lookup lwIP could not cancel fires after the call that started it has
    // returned, and the slot it writes into has to still be there (AR-6).
    // The transport is constinit. The synchronizer is initialized on first
    // mainline use, before callbacks can reach its state.
    static constinit LwipNtpTransport transport{};
    static NtpSynchronizer synchronizer{transport, NTP_SERVER};

    const pico_util::Result<SynchronizedTime, NtpError> result =
        synchronizer.synchronize(timeout);
    if (!result.ok()) {
        PICO_LOG_WARN << "NTP: " << describe(result.error());
        return result.error();
    }

    const SynchronizedTime sample = result.value();
    const pico_util::Result<void, WallClockError> applied =
        apply_wall_clock_time(sample.epoch_seconds, sample.epoch_microseconds);
    if (!applied.ok()) {
        PICO_LOG_ERROR << "NTP: Failed to apply system time: "
                       << describe(applied.error());
        return NtpError::CLOCK_REFUSED;
    }

    log_synchronized(sample);
    return {};
}

}  // namespace pico_time
