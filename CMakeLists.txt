cmake_minimum_required(VERSION 3.26)

include(cmake/pico_family_bootstrap.cmake)

# CYW43 supplies the radio. pico-logging requires RP2350 (AR-7).
pico_family_bootstrap(PICO_TIME PLATFORM rp2350 CYW43)

project(pico_time VERSION 0.2.0 LANGUAGES C CXX ASM)

house_cxx_common_settings(${PICO_TIME_BUILD_FIRMWARE})
pico_family_init_sdk(${PICO_TIME_BUILD_FIRMWARE})

# Firmware -Werror applies per source to exclude SDK sources. Hosted hardening
# follows the toolchain. Only project-owned C++ needs strict diagnostics.
house_cxx_add_strict_target(pico_time_strict
    VENDOR_SOURCES ${PICO_TIME_BUILD_FIRMWARE}
    LANGUAGES CXX
)

# Sanitizers precede GoogleTest so both sides of container boundaries
# are instrumented.
house_cxx_enable_sanitizers(PICO_TIME)

# Options are declared before any subdirectory reads them.
house_cxx_declare_tests_option(PICO_TIME)

# Fuzzing is declared here so an unsupported configuration reports an error.
house_cxx_declare_fuzzers_option(PICO_TIME)

# Device-validation examples need the firmware tree.
set(PICO_TIME_ENABLE_EXAMPLES_DEFAULT OFF)
if(PROJECT_IS_TOP_LEVEL AND PICO_TIME_BUILD_FIRMWARE)
    set(PICO_TIME_ENABLE_EXAMPLES_DEFAULT ON)
endif()
option(PICO_TIME_ENABLE_EXAMPLES "Build the firmware examples"
    ${PICO_TIME_ENABLE_EXAMPLES_DEFAULT})
# Document probes are built only by this project's firmware configure.
pico_family_declare_probe_option(PICO_TIME)

# Installed archives are cross-compiled. Host tests use in-tree targets.
house_cxx_declare_install_option(PICO_TIME AVAILABLE ${PICO_TIME_BUILD_FIRMWARE})

# Install settings propagate before dependency configuration. Exported
# static libraries must have their dependencies in an export set too.
pico_family_propagate_install(PICO_TIME PICO_LOGGING)
pico_family_propagate_install(PICO_TIME PICO_UTIL)

# Both host and firmware NTP modules return pico_util::Result.
pico_family_dependency(pico_util
    REPOSITORY https://github.com/jamesfxley/pico-util.git
    REVISION 870e68b33012c9052d3336be4ad9abd58d44d4d1)
pico_family_make_available(pico_util)

# The portable modules do not depend on the logger.
if(PICO_TIME_BUILD_FIRMWARE)
    pico_family_dependency(pico_logging
        REPOSITORY https://github.com/jamesfxley/pico-logging.git
        REVISION 724f4555863d60fb68b1ddff944c0ce89398c16a)
    pico_family_make_available(pico_logging)
endif()

set(PICO_LWIPOPTS_TARGET "" CACHE STRING "Target carrying the application's lwipopts.h")
set(PICO_TIME_WIFI_SSID "" CACHE STRING "Wi-Fi SSID the example firmware should join")
set(PICO_TIME_WIFI_PASSWORD "" CACHE STRING "Wi-Fi password (WPA2-PSK)")

# One options target configures the archive and image (VR-7).
# Only this explicit target enables the local fixtures.
if(PROJECT_IS_TOP_LEVEL AND PICO_TIME_BUILD_FIRMWARE)
    add_library(pico_time_device_lwipopts INTERFACE)
    target_link_libraries(pico_time_device_lwipopts INTERFACE
        pico_time_lwipopts_fallback)
    target_compile_definitions(pico_time_device_lwipopts INTERFACE
        LWIP_NETIF_LOOPBACK=1 LWIP_HAVE_LOOPIF=1 LWIP_SINGLE_NETIF=0
        LWIP_NETIF_LOOPBACK_MULTITHREADING=0 MEMP_NUM_UDP_PCB=8
        DNS_LOCAL_HOSTLIST=1
        "DNS_LOCAL_HOSTLIST_INIT={DNS_LOCAL_HOSTLIST_ELEM(\"pool.ntp.org\",IPADDR4_INIT_BYTES(127,0,0,1))}")
endif()

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

if(PROJECT_IS_TOP_LEVEL AND PICO_TIME_BUILD_FIRMWARE)
    add_subdirectory(tests/device)
endif()

if(PICO_TIME_ENABLE_INSTALL)
    if(NOT PICO_TIME_BUILD_FIRMWARE)
        message(FATAL_ERROR
            "PICO_TIME_ENABLE_INSTALL requires a firmware configure: the "
            "exported headers belong to pico_time_time_sync, which needs the "
            "Pico SDK. A host build has no public surface to install.")
    endif()
    # pico_time_strict and the fallback are exported because static-library
    # PRIVATE dependencies stay in the $<LINK_ONLY:...> interface.
    # The installed fake serves firmware tests; host tests use in-tree targets.
    set(PICO_TIME_EXPORT_TARGETS
        pico_time_strict pico_time_core pico_time_ntp pico_time_time_sync
        pico_time_testing pico_time_lwipopts_fallback)
    # Internal fixture configuration closes the static archive's link interface.
    if(PICO_LWIPOPTS_TARGET STREQUAL "pico_time_device_lwipopts")
        list(APPEND PICO_TIME_EXPORT_TARGETS pico_time_device_lwipopts)
    endif()
    pico_family_sdk_config_snippet(PICO_TIME_SDK_CONFIG TARGETS ${PICO_TIME_EXPORT_TARGETS})
    house_cxx_install_package(
        PACKAGE pico_time VERSION ${PROJECT_VERSION}
        CONFIG_SNIPPET "${PICO_TIME_SDK_CONFIG}"
        NAMESPACE pico_time
        TARGETS ${PICO_TIME_EXPORT_TARGETS}
        DEPENDS pico_logging pico_util
    )
endif()
