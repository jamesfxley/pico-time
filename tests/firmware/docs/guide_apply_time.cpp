// SPDX-License-Identifier: MIT
// Setting the clock from an epoch time the caller already has.
#include <cstdint>
#include <pico_logging/pico_logging.hpp>
#include <pico_time/time_sync/time_sync.hpp>

namespace doc_examples {
void guide_apply_time(std::uint64_t seconds, std::uint32_t microseconds) {
#include "guide_apply_time.inc"
}
}  // namespace doc_examples
