// SPDX-License-Identifier: MIT
// Host tests check the header version against CMake.

#include "pico_time/pico_time.hpp"

#include <gtest/gtest.h>

namespace {

TEST(Version, MatchesTheVersionCMakeHolds) {
    EXPECT_EQ(pico_time::VERSION_MAJOR, PICO_TIME_CMAKE_VERSION_MAJOR);
    EXPECT_EQ(pico_time::VERSION_MINOR, PICO_TIME_CMAKE_VERSION_MINOR);
    EXPECT_EQ(pico_time::VERSION_PATCH, PICO_TIME_CMAKE_VERSION_PATCH);
}

}  // namespace
