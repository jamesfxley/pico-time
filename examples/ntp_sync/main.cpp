// SPDX-License-Identifier: MIT
// Device validation for pre-sync reads, monotonic reads and repeat calls
// (FR-2, FR-7, NR-4). Host tests cover retries (NR-3).
// Capture serial output before reset. All output occurs at boot.

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

    // Conversion failure prints an unavailable marker, not a plausible wrong
    // time.
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

/// Keep USB servicing active without returning from main().
[[noreturn]] void park(const char* reason) {
    PICO_LOG_INFO << "Parked (" << reason << "); USB stays live for reflash";
    while (true) {
        sleep_ms(PARK_TICK_MS);
    }
}

bool join_wifi() {
    // An explicit length avoids a redundant-initialisation diagnostic when
    // the macro is "".
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

    // misc-include-cleaner does not attribute the SDK auth macro to its header.
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

    // Pre-sync epoch seconds must be small, counting from boot (FR-2).
    PICO_LOG_INFO << "synchronized=" << pico_time::is_time_synchronized();
    log_wall_clock("pre-sync");

    if (!join_wifi()) {
        park("Wi-Fi association failed");
    }

    // Up to three bounded attempts report their error and retryability (NR-3,
    // AR-6).
    if (const auto synced = pico_time::synchronize_ntp(); !synced.ok()) {
        PICO_LOG_ERROR << "NTP synchronization failed: "
                       << pico_time::describe(synced.error())
                       << (pico_time::retryable(synced.error())
                               ? " (retryable)"
                               : " (not retryable)");
        park("NTP synchronization failed");
    }
    log_wall_clock("post-sync");

    // With the clock set, this call must succeed immediately and log no DNS or
    // NTP activity (NR-4).
    PICO_LOG_INFO << "Re-synchronizing (expect an immediate ok, no traffic)";
    const auto again = pico_time::synchronize_ntp();
    PICO_LOG_INFO << "second synchronize_ntp() ok=" << again.ok();

    // Between sets, the clock advances without stepping (FR-7).
    sleep_ms(OBSERVATION_DELAY_MS);
    log_wall_clock("after 3s");

    PICO_LOG_INFO << "Example complete";
    park("example complete");
}
