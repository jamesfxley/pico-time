// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

struct LoopbackResult {
    bool passed = false;
    std::string_view detail;
};

// Public synchronisation runs through the lwIP loopback fixture (VR-7).
[[nodiscard]] LoopbackResult check_loopback_replies();
