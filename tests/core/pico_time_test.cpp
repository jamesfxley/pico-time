// SPDX-License-Identifier: MIT
// The host-side half of the version pin.
//
// The advertised version and project(pico_time VERSION ...) are two records of
// one fact. time_keeper.cpp static_asserts them against each other in the
// firmware build; this does the same on the host, so the drift is caught by
// whichever tree gets built first rather than only by the one that needs an
// SDK.

#include "pico_time/pico_time.hpp"

#include <gtest/gtest.h>

namespace {

TEST(Version, MatchesTheVersionCMakeHolds) {
    EXPECT_EQ(pico_time::VERSION_MAJOR, PICO_TIME_CMAKE_VERSION_MAJOR);
    EXPECT_EQ(pico_time::VERSION_MINOR, PICO_TIME_CMAKE_VERSION_MINOR);
    EXPECT_EQ(pico_time::VERSION_PATCH, PICO_TIME_CMAKE_VERSION_PATCH);
}

}  // namespace
