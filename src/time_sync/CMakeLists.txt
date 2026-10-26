add_library(pico_time_time_sync STATIC
    ntp_client.cpp
    lwip_ntp_transport.cpp
    time_keeper.cpp
)
add_library(pico_time::time_sync ALIAS pico_time_time_sync)

# Warnings are errors for the code this project owns (EN-01.3). Per source
# rather than on the target: the SDK compiles its own sources into this one.
# A source added here and not registered below is caught by the CI build-log
# grep, which is the backstop rather than the gate.
house_cxx_werror_sources(
    ntp_client.cpp
    lwip_ntp_transport.cpp
    time_keeper.cpp
    ../core/ntp_message.cpp
    ../core/wall_clock.cpp
    ../ntp/ntp_synchronizer.cpp
)

# Two BASE_DIRS, because this target carries headers belonging to two places:
# its own module, and the umbrella at src/include/, which belongs to no module
# (CXX-13.1) but has to be installed by some target.
target_sources(pico_time_time_sync PUBLIC
    FILE_SET HEADERS
    BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PICO_TIME_PUBLIC_INCLUDE_DIR}
    FILES
        include/pico_time/time_sync/time_sync.hpp
        ${PICO_TIME_PUBLIC_INCLUDE_DIR}/pico_time/pico_time.hpp
)

# time_keeper.cpp static_asserts the header's advertised version against these,
# so the two records of the version cannot drift; tests/core/pico_time_test.cpp pins
# the same constants from the host side.
house_cxx_version_definitions(pico_time_time_sync pico_time PRIVATE)

# core's ntp_message.hpp and wall_clock.hpp are its implementation detail: in no
# FILE_SET, never installed. This module uses them, so it names core's source
# directory PRIVATE. That is deliberately not the same as core exporting it --
# on core's PUBLIC interface the same headers would be reachable by any in-tree
# consumer of pico_time::core and absent from the installed package, so a
# consumer including one would build here and fail downstream.
target_include_directories(pico_time_time_sync PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core>
)

target_include_directories(pico_time_time_sync PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PICO_TIME_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_time_time_sync PUBLIC cxx_std_20)

# core is PUBLIC, not PRIVATE: this module's public header includes
# <pico_time/core/epoch.hpp>, so a consumer of time_sync.hpp needs core's
# include directory. Both targets are in the export set, so the installed
# package carries the same relationship.
# ntp is PUBLIC: time_sync.hpp returns a pico_util::Result over this library's
# NtpError, both of which arrive through that target.
target_link_libraries(pico_time_time_sync
    PUBLIC
        pico_logging::core
        pico_time::core
        pico_time::ntp
    PRIVATE
        pico_time_strict
)

# The SDK libraries go through this rather than target_link_libraries, because a
# target linking them cannot be exported: no SDK target is in any export set,
# and both export(TARGETS) and install(EXPORT) refuse a target whose link
# interface names one. They are wrapped in $<BUILD_LOCAL_INTERFACE:> and
# re-applied by the installed config file, once the consumer's own
# pico_sdk_init() has created them.
pico_family_sdk_link_libraries(pico_time_time_sync
    PUBLIC
        pico_stdlib
        pico_lwip_nosys
        pico_cyw43_arch
        pico_cyw43_arch_lwip_threadsafe_background
    PRIVATE
        pico_rand
)

target_link_libraries(pico_time_time_sync PRIVATE ${PICO_LWIPOPTS_TARGET})
