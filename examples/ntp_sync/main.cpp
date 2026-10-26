// SPDX-License-Identifier: MIT
// Device validation for pico-time.
//
// Establishes the traceability rows the host suite cannot reach, because they
// are properties of the SDK counter, the radio and the newlib link rather than
// of the portable core: FR-2 (pre-sync reads count from boot), FR-7 (the clock
// advances monotonically between syncs) and NR-4 (an already-synchronized call
// returns without traffic). NR-3 is not among them: the retry structure is
// transport-independent and is established on the host, and this example keeps
// no capture of it -- see the README for the local edit that makes it
// observable.
//
// Read the serial log from before the reset: the whole sequence is logged at
// boot and the board is quiet afterwards.

// The specific SDK headers rather than the pico/stdlib.h umbrella: they are
// public headers, and naming them is what lets include-cleaner check this file
// instead of reporting the umbrella as unused.
#include <pico/cyw43_arch.h>
#include <pico/stdio.h>
#include <pico/time.h>

#include <array>
#include <cstdint>
#include <ctime>
#include <pico_time/time_sync/time_sync.hpp>
#include <string_view>

#include "pico_logging/logging/logging.hpp"
#include "pico_time/pico_time.hpp"

namespace {

constexpr int WIFI_CONNECT_TIMEOUT_MS = 30000;
constexpr int USB_ENUMERATION_DELAY_MS = 2000;
constexpr int OBSERVATION_DELAY_MS = 3000;
constexpr std::size_t TIME_STRING_SIZE = 32;
constexpr int PARK_TICK_MS = 1000;

void log_wall_clock(const char* label) {
    const std::time_t now = std::time(nullptr);
    struct tm utc{};
    std::array<char, TIME_STRING_SIZE> formatted{};

    // A failed conversion, or a failed format, leaves the marker rather than a
    // plausible-looking wrong time.
    if (gmtime_r(&now, &utc) == nullptr ||
        std::strftime(formatted.data(), formatted.size(), "%Y-%m-%d %H:%M:%S",
                      &utc) == 0) {
        PICO_LOG_INFO << label << ": epoch=" << static_cast<std::int64_t>(now)
                      << " (unavailable)";
        return;
    }
    PICO_LOG_INFO << label << ": epoch=" << static_cast<std::int64_t>(now)
                  << " (" << formatted.data() << " UTC)";
}

/// Stop here without returning from main().
///
/// Returning ends the program, and with it the SDK's servicing of USB: the tail
/// of the log is lost, and the 1200-baud BOOTSEL touch stops working, so a
/// board that failed early can only be recovered by physically replugging it.
/// Parking keeps the port alive and the board reflashable.
[[noreturn]] void park(const char* reason) {
    PICO_LOG_INFO << "Parked (" << reason << "); USB stays live for reflash";
    while (true) {
        sleep_ms(PARK_TICK_MS);
    }
}

bool join_wifi() {
    // PICO_TIME_WIFI_SSID is a compile definition and is empty only in an
    // unconfigured build, which is worth saying plainly rather than failing to
    // associate. Constructed with an explicit length from the literal: in an
    // unconfigured build the macro is "", and a single-argument construction
    // from it reads to analysis as a redundant initialization.
    constexpr std::string_view SSID(PICO_TIME_WIFI_SSID,
                                    sizeof(PICO_TIME_WIFI_SSID) - 1);
    if (SSID.empty()) {
        PICO_LOG_ERROR << "PICO_TIME_WIFI_SSID is empty. Configure with: "
                          "cmake -B build-firmware -DPICO_TIME_WIFI_SSID=... "
                          "-DPICO_TIME_WIFI_PASSWORD=...";
        return false;
    }

    cyw43_arch_enable_sta_mode();
    PICO_LOG_INFO << "Joining " << SSID.data();

    // include-cleaner does not count a macro expansion as a use of its header,
    // so it reports no header providing the SDK's auth-mode constant.
    // NOLINTNEXTLINE(misc-include-cleaner)
    constexpr std::uint32_t AUTH_MODE = CYW43_AUTH_WPA2_AES_PSK;
    if (cyw43_arch_wifi_connect_timeout_ms(PICO_TIME_WIFI_SSID,
                                           PICO_TIME_WIFI_PASSWORD, AUTH_MODE,
                                           WIFI_CONNECT_TIMEOUT_MS) != 0) {
        PICO_LOG_ERROR << "Wi-Fi association failed";
        return false;
    }
    PICO_LOG_INFO << "Wi-Fi up";
    return true;
}

}  // namespace

int main() {
    stdio_init_all();
    sleep_ms(USB_ENUMERATION_DELAY_MS);

    PICO_LOG_INFO << "pico-time example, v" << pico_time::VERSION_MAJOR << "."
                  << pico_time::VERSION_MINOR << "."
                  << pico_time::VERSION_PATCH;

    if (cyw43_arch_init() != 0) {
        PICO_LOG_ERROR << "cyw43_arch_init failed";
        park("cyw43_arch_init failed");
    }

    // FR-2: before any synchronization these read as time since boot, so the
    // epoch value here must be small (single-digit seconds), not a 2020s date.
    PICO_LOG_INFO << "synchronized=" << pico_time::is_time_synchronized();
    log_wall_clock("pre-sync");

    if (!join_wifi()) {
        park("Wi-Fi association failed");
    }

    // NR-3: on failure this is three attempts, each bounded by the timeout.
    // The error says which failure it was, and whether retrying could clear
    // it (AR-6).
    if (const auto synced = pico_time::synchronize_ntp(); !synced.ok()) {
        PICO_LOG_ERROR << "NTP synchronization failed: "
                       << pico_time::describe(synced.error())
                       << (pico_time::retryable(synced.error())
                               ? " (retryable)"
                               : " (not retryable)");
        park("NTP synchronization failed");
    }
    log_wall_clock("post-sync");

    // NR-4: already synchronized, so this must succeed immediately and log no
    // DNS or NTP activity at all.
    PICO_LOG_INFO << "Re-synchronizing (expect an immediate ok, no traffic)";
    const auto again = pico_time::synchronize_ntp();
    PICO_LOG_INFO << "second synchronize_ntp() ok=" << again.ok();

    // FR-7: the clock advances with the monotonic counter and does not step.
    sleep_ms(OBSERVATION_DELAY_MS);
    log_wall_clock("after 3s");

    PICO_LOG_INFO << "Example complete";
    park("example complete");
}
