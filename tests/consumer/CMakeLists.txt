cmake_minimum_required(VERSION 3.26)

# Stands in for a downstream firmware project consuming an installed pico-time.
#
# The SDK is initialised BEFORE find_package, which is the contract the
# installed config file asserts: pico_time links Pico SDK libraries, those
# cannot be exported, and pico_timeConfig.cmake re-applies them by name. If the
# consumer has not created those targets first, the config says so rather than
# leaving an undefined symbol for the linker.
#
# Nothing here reaches into the pico-time source tree. The headers, the
# lwipopts fallback and the find_dependency(pico_logging) chain must all arrive
# through the installed package.
include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(pico_time_consumer_smoke LANGUAGES C CXX ASM)

pico_sdk_init()

# House supplies the relocated prefix through CMAKE_PREFIX_PATH. The SDK's
# cross toolchain restricts package lookup to its roots; search this explicit
# host-side installation prefix as well. Package registries remain disabled.
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

find_package(pico_time REQUIRED)

add_executable(consumer_smoke consumer_smoke.cxx)

# lwipopts_fallback is exported too, so a consumer with no lwIP configuration of
# its own can still link. A real application would supply its own.
# Every exported target is named, not only the aggregate. The aggregate pulls
# most of them in transitively, so a module dropped from the export set would
# still resolve here and the smoke would report nothing -- which is the one
# contract this executable exists to prove.
target_link_libraries(consumer_smoke PRIVATE
    pico_time::core
    pico_time::ntp
    pico_time::time_sync
    pico_time::testing
    pico_time::lwipopts_fallback
)

target_compile_features(consumer_smoke PRIVATE cxx_std_20)
