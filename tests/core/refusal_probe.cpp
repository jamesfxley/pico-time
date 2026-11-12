// SPDX-License-Identifier: MIT
// The default probe compiles. PROBE=1 discards a nodiscard bool result (AR-1).
#include "pico_time/time_sync/time_sync.hpp"

namespace {

[[maybe_unused]] bool synchronized_probe() {
#if defined(PROBE) && PROBE == 1
    pico_time::is_time_synchronized();
#endif
    return pico_time::is_time_synchronized();
}

}  // namespace
