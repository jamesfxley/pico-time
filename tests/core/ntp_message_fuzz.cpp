// SPDX-License-Identifier: MIT
// ASan and UBSan detect undefined behaviour while parsing arbitrary NTP bytes.

#include <cstddef>
#include <cstdint>
#include <pico_time/core/epoch.hpp>
#include <span>

#include "ntp_message.hpp"
#include "wall_clock.hpp"

// readability-identifier-naming: the libFuzzer runtime fixes this entry name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    // The fixed-extent parser requires a complete header.
    if (size < pico_time::NTP_MESSAGE_LENGTH) {
        return 0;
    }

    const std::span<const std::uint8_t, pico_time::NTP_MESSAGE_LENGTH> message(
        data, pico_time::NTP_MESSAGE_LENGTH);

    // An input-derived nonce lets fuzzing reach beyond nonce rejection.
    std::uint64_t nonce = 0;
    for (std::size_t i = 0; i < sizeof(nonce); ++i) {
        nonce = (nonce << 8U) | message[i];
    }

    const pico_time::NtpParse parse =
        pico_time::parse_ntp_response(message, nonce);
    if (parse.sample.has_value()) {
        // An accepted sample must satisfy the epoch and microsecond bounds.
        if (parse.sample->epoch_microseconds > pico_time::MAX_MICROSECONDS) {
            __builtin_trap();
        }
        if (parse.sample->epoch_seconds < pico_time::MIN_EPOCH_SECONDS ||
            parse.sample->epoch_seconds >= pico_time::MAX_EPOCH_SECONDS) {
            __builtin_trap();
        }
    }
    return 0;
}
