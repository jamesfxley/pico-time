// SPDX-License-Identifier: MIT
// Consumer-side view of the installed package. Every symbol below must arrive
// through the installed pico_time::time_sync target, resolved by find_package,
// so a header the package fails to ship is a compile error here rather than a
// discovery made by the first downstream project.
//
// This is a firmware translation unit, and it has to be: pico-time's public
// surface links lwIP and the CYW43 driver, so the only consumer that can
// exercise it is one that cross-compiles. That is the contract under test.
//
// It is not run. Linking is the assertion -- it proves the SDK libraries the
// installed config re-applies by name actually resolved, which is the whole
// point of pico_family_sdk_link_libraries.
#include <chrono>
#include <cstdint>

#include "pico_time/pico_time.hpp"

static_assert(pico_time::VERSION_MAJOR == 0 && pico_time::VERSION_MINOR == 1);

#include <pico_time/ntp/ntp_error.hpp>
#include <pico_time/ntp/ntp_synchronizer.hpp>
#include <pico_time/testing/fake_ntp_transport.hpp>

// The validation window is part of the exported surface, not an internal.
static_assert(pico_time::MAX_EPOCH_SECONDS > pico_time::MIN_EPOCH_SECONDS);

// The transport seam and its double are part of the exported surface: a
// downstream project tests its own code against the same fake this repository's
// tests use. An export set that drops pico_time::testing fails to compile here.
namespace {
pico_time::testing::FakeNtpTransport fake_transport;
}  // namespace

int main() {
    // Every entry point named, so a symbol dropped from the installed library
    // is a link error here.
    if (pico_time::is_time_synchronized()) {
        return 0;
    }
    if (pico_time::apply_wall_clock_time(pico_time::MIN_EPOCH_SECONDS, 0U)
            .ok()) {
        return 0;
    }

    // The policy is reachable through the installed package, over the installed
    // fake, with no network and no SDK call.
    pico_time::NtpSynchronizer synchronizer(fake_transport, "pool.ntp.org");
    if (synchronizer.synchronize(std::chrono::milliseconds(1)).ok()) {
        return 0;
    }
    // describe() and retryable() are the taxonomy's own accessors; naming them
    // keeps them in the exported surface too.
    if (pico_time::retryable(pico_time::NtpError::NO_ATTEMPT_SLOT)) {
        static_cast<void>(pico_time::describe(pico_time::NtpError::TIMED_OUT));
    }

    return pico_time::synchronize_ntp(std::chrono::seconds(1)).ok() ? 0 : 1;
}
