cmake_minimum_required(VERSION 3.26)

include(cmake/pico_family_bootstrap.cmake)

# AR-7. Synchronisation runs over the CYW43 radio, and pico-logging needs an
# RP2350. As a dependency the board is the consumer's, and only that is checked
# of it.
pico_family_bootstrap(PICO_TIME PLATFORM rp2350 CYW43)

project(pico_time VERSION 0.1.0 LANGUAGES C CXX ASM)

house_cxx_common_settings(${PICO_TIME_BUILD_FIRMWARE})
pico_family_init_sdk(${PICO_TIME_BUILD_FIRMWARE})

# VENDOR_SOURCES takes the firmware flag: the Pico SDK compiles its own sources
# into whichever target links its INTERFACE libraries, so a target-scoped
# -Werror there would land on SDK and lwIP code this repo cannot fix. In the
# firmware tree it goes back on per source, in src/time_sync/CMakeLists.txt.
# The host tree links no SDK sources, so the target carries it directly.
#
# FREESTANDING follows the toolchain, which is the helper's default: the
# portable core (wire format, clock arithmetic) is host-compiled for the test
# suite, and a hosted build does have the libc behind _FORTIFY_SOURCE and the
# dynamic loader behind the RELRO link options that a firmware image does not.
#
# LANGUAGES CXX because every source this repo owns is C++, and C diagnostics
# here could only ever report on SDK code.
house_cxx_add_strict_target(pico_time_strict
    VENDOR_SOURCES ${PICO_TIME_BUILD_FIRMWARE}
    LANGUAGES CXX
)

# Declares PICO_TIME_SANITIZE. Before the targets and before GoogleTest, and
# build-wide rather than target-scoped, so the fetched framework is instrumented
# too -- ASan's container-overflow detector needs both sides of a std::string
# boundary built alike.
house_cxx_enable_sanitizers(PICO_TIME)

# Every knob this build has, declared before anything is added: a subdirectory
# reading an option that has not been declared yet reads an empty string.
house_cxx_declare_tests_option(PICO_TIME)

# libFuzzer targets (Clang only). Declared here rather than beside the targets
# themselves so that enabling it on a configure that cannot honour it reports
# the reason instead of silently building nothing.
house_cxx_declare_fuzzers_option(PICO_TIME)

# Firmware-only: the examples exist to establish the traceability rows whose
# criterion is a device run, so there is nothing to build for them on a host.
set(PICO_TIME_ENABLE_EXAMPLES_DEFAULT OFF)
if(PROJECT_IS_TOP_LEVEL AND PICO_TIME_BUILD_FIRMWARE)
    set(PICO_TIME_ENABLE_EXAMPLES_DEFAULT ON)
endif()
option(PICO_TIME_ENABLE_EXAMPLES "Build the firmware examples"
    ${PICO_TIME_ENABLE_EXAMPLES_DEFAULT})
# The documents' firmware examples (tests/firmware), built only by this
# project's own cross configure, never by a consumer that fetches it.
pico_family_declare_probe_option(PICO_TIME)

# Install/export is firmware-only: the package is one cross-compiled export
# set. Portable core and NTP modules have public headers; their host tests use
# the in-tree targets, not the installed ARM archives.
house_cxx_declare_install_option(PICO_TIME AVAILABLE ${PICO_TIME_BUILD_FIRMWARE})

# When this project's targets are exported, the fetched dependencies they link
# must be in an export set too. Before the fetch below, or the dependency is
# configured with its default first.
pico_family_propagate_install(PICO_TIME PICO_LOGGING)
pico_family_propagate_install(PICO_TIME PICO_UTIL)

# pico_util carries Result, the container the NTP error taxonomy is returned in
# (CXX-07.1: the carrier is generic, the vocabulary is this library's). Fetched
# unconditionally because pico_time::ntp is the portable half and is built and
# tested on a host, where the firmware branch below never runs.
pico_family_dependency(pico_util
    REPOSITORY https://github.com/jamesfxley/pico-util.git
    REVISION 2afe3150a52429cb61c2330041cb4ec2e26c00ef)
pico_family_make_available(pico_util)

# Only the lwIP transport needs the logger; the portable half has no dependency
# on it, which is what lets that half build and be tested on a host.
if(PICO_TIME_BUILD_FIRMWARE)
    pico_family_dependency(pico_logging
        REPOSITORY https://github.com/jamesfxley/pico-logging.git
        REVISION fe23964417288a0dedfa27ddbd7e2f85b0915bae)
    pico_family_make_available(pico_logging)
endif()

set(PICO_LWIPOPTS_TARGET "" CACHE STRING "Target carrying the application's lwipopts.h")
set(PICO_TIME_WIFI_SSID "" CACHE STRING "Wi-Fi SSID the example firmware should join")
set(PICO_TIME_WIFI_PASSWORD "" CACHE STRING "Wi-Fi password (WPA2-PSK)")

add_subdirectory(src)

if(PICO_TIME_ENABLE_TESTS)
    house_cxx_enable_gtest()
    add_subdirectory(tests)
endif()

if(PICO_TIME_ENABLE_FIRMWARE_PROBE)
    add_subdirectory(tests/firmware)
endif()

if(PICO_TIME_ENABLE_EXAMPLES)
    add_subdirectory(examples)
endif()

if(PICO_TIME_ENABLE_INSTALL)
    if(NOT PICO_TIME_BUILD_FIRMWARE)
        message(FATAL_ERROR
            "PICO_TIME_ENABLE_INSTALL requires a firmware configure: the "
            "exported headers belong to pico_time_time_sync, which needs the "
            "Pico SDK. A host build has no public surface to install.")
    endif()
    # pico_time_lwipopts_fallback is in the set because pico_time_time_sync
    # links it PRIVATE when it is the chosen lwipopts target, and a PRIVATE
    # dependency of a static library still appears in the link interface under
    # $<LINK_ONLY:...>. Same reason pico_time_strict is in it.
    #
    # pico_time_testing lets a downstream on-target test use this fake. Host
    # tests
    # consume it in-tree through FetchContent; the installed export is ARM.
    set(PICO_TIME_EXPORT_TARGETS
        pico_time_strict pico_time_core pico_time_ntp pico_time_time_sync
        pico_time_testing pico_time_lwipopts_fallback)
    pico_family_sdk_config_snippet(PICO_TIME_SDK_CONFIG TARGETS ${PICO_TIME_EXPORT_TARGETS})
    house_cxx_install_package(
        PACKAGE pico_time VERSION ${PROJECT_VERSION}
        CONFIG_SNIPPET "${PICO_TIME_SDK_CONFIG}"
        NAMESPACE pico_time
        TARGETS ${PICO_TIME_EXPORT_TARGETS}
        DEPENDS pico_logging pico_util
    )
endif()
