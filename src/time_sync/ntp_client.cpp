// SPDX-License-Identifier: MIT

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

// The DNS API needs a NUL-terminated name.
constexpr const char* NTP_SERVER = "pool.ntp.org";
constexpr std::size_t TIME_STRING_SIZE = 20;  // "YYYY-MM-DD HH:MM:SS" + NUL

void log_synchronized(const SynchronizedTime& sample) {
    const auto epoch_seconds = static_cast<time_t>(sample.epoch_seconds);

    // Failed conversion leaves a zeroed tm that would render as a 1900 date.
    // LogLine cannot format the std::put_time proxy.
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

    // The slot pool must outlive this call for uncancellable DNS callbacks
    // (AR-6). The synchronizer initialises on first mainline use, before any
    // callback.
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
