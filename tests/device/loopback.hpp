// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

struct LoopbackResult {
    bool passed = false;
    std::string_view detail;
};

// VR-7: real public synchronization through the lwIP loopback fixture.
[[nodiscard]] LoopbackResult check_loopback_replies();
