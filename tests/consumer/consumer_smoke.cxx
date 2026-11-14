// SPDX-License-Identifier: MIT
// The firmware consumer must obtain every symbol from the installed package.
// Linking checks SDK dependency resolution; this executable is not run.
#include <chrono>
#include <cstdint>

#include "pico_time/pico_time.hpp"

static_assert(pico_time::VERSION_MAJOR == 0 && pico_time::VERSION_MINOR == 2);

#include <pico_time/ntp/ntp_error.hpp>
#include <pico_time/ntp/ntp_synchronizer.hpp>
#include <pico_time/testing/fake_ntp_transport.hpp>

// The validation window is part of the exported surface, not an internal.
static_assert(pico_time::MAX_EPOCH_SECONDS > pico_time::MIN_EPOCH_SECONDS);

// The installed fake exposes the transport interface to downstream tests.
namespace {
pico_time::testing::FakeNtpTransport fake_transport;
}  // namespace

int main() {
    // Referencing each entry point detects missing library symbols.
    if (pico_time::is_time_synchronized()) {
        return 0;
    }
    if (pico_time::apply_wall_clock_time(pico_time::MIN_EPOCH_SECONDS, 0U)
            .ok()) {
        return 0;
    }

    // The installed fake exercises the installed policy.
    pico_time::NtpSynchronizer synchronizer(fake_transport, "pool.ntp.org");
    if (synchronizer.synchronize(std::chrono::milliseconds(1)).ok()) {
        return 0;
    }
    // Referencing the accessors checks their exported definitions.
    if (pico_time::retryable(pico_time::NtpError::NO_ATTEMPT_SLOT)) {
        static_cast<void>(pico_time::describe(pico_time::NtpError::TIMED_OUT));
    }

    return pico_time::synchronize_ntp(std::chrono::seconds(1)).ok() ? 0 : 1;
}
