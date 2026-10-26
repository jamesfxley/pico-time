// SPDX-License-Identifier: MIT
// libFuzzer over the NTP trust boundary.
//
// parse_ntp_response() is the one place in this library that reads bytes an
// attacker chose, so it is the one place EN-04.6 asks to be fuzzed. The
// property under test is simply that no 48-byte input reaches undefined
// behaviour -- ASan and UBSan supply the oracle.

#include <cstddef>
#include <cstdint>
#include <pico_time/core/epoch.hpp>
#include <span>

#include "ntp_message.hpp"
#include "wall_clock.hpp"

// The name is libFuzzer's entry point, fixed by the runtime that calls it, not
// a style choice -- the same situation as the newlib syscall shims.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    // A short read is refused by the caller before it ever reaches the parser,
    // which takes a span of static extent precisely so the length cannot be
    // got wrong here either.
    if (size < pico_time::NTP_MESSAGE_LENGTH) {
        return 0;
    }

    // The libFuzzer entry point is handed a pointer and a length; the span is
    // where that ends, since everything downstream carries bounds in the type.
    const std::span<const std::uint8_t, pico_time::NTP_MESSAGE_LENGTH> message(
        data, pico_time::NTP_MESSAGE_LENGTH);

    // The nonce is drawn from the message itself, so the corpus can reach both
    // sides of the originate check rather than only the mismatch path.
    std::uint64_t nonce = 0;
    for (std::size_t i = 0; i < sizeof(nonce); ++i) {
        nonce = (nonce << 8U) | message[i];
    }

    const pico_time::NtpParse parse =
        pico_time::parse_ntp_response(message, nonce);
    if (parse.sample.has_value()) {
        // An accepted sample must satisfy what the header promises about it;
        // a violation here is a parser defect the sanitizers cannot see.
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
