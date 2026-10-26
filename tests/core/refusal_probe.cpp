// SPDX-License-Identifier: MIT
// AR-1 as a build failure. With PROBE undefined this compiles and is part of
// every build, so the probe itself is sound; PROBE=1 discards the one public
// entry point whose status is a bool rather than a pico_util::Result, which
// the library promises to refuse.
#include "pico_time/time_sync/time_sync.hpp"

namespace {

[[maybe_unused]] bool synchronized_probe() {
#if defined(PROBE) && PROBE == 1
    pico_time::is_time_synchronized();
#endif
    return pico_time::is_time_synchronized();
}

}  // namespace
