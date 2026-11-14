cmake_minimum_required(VERSION 3.26)

# Stands in for a downstream firmware project consuming an installed pico-time.
#
# The SDK is initialised before find_package so the installed config can
# reapply its SDK link targets.
#
# Headers, fallback options and dependencies must come from the installed package.
include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(pico_time_consumer_smoke LANGUAGES C CXX ASM)

pico_sdk_init()

# House supplies the relocated prefix through CMAKE_PREFIX_PATH. The SDK's
# cross toolchain restricts package lookup to its roots; search this explicit
# host-side installation prefix as well. Package registries remain disabled.
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

find_package(pico_time REQUIRED)

add_executable(consumer_smoke consumer_smoke.cxx)

# Every exported module is named so a missing export fails configuration.
# The fallback serves consumers with no lwIP configuration of their own.
target_link_libraries(consumer_smoke PRIVATE
    pico_time::core
    pico_time::ntp
    pico_time::time_sync
    pico_time::testing
    pico_time::lwipopts_fallback
)

target_compile_features(consumer_smoke PRIVATE cxx_std_20)
