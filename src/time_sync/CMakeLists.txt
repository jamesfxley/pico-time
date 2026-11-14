add_library(pico_time_time_sync STATIC
    ntp_client.cpp
    lwip_ntp_transport.cpp
    time_keeper.cpp
)
add_library(pico_time::time_sync ALIAS pico_time_time_sync)

# Per-source warning flags exclude SDK sources compiled into this target.
house_cxx_werror_sources(
    ntp_client.cpp
    lwip_ntp_transport.cpp
    time_keeper.cpp
    ../core/ntp_message.cpp
    ../core/wall_clock.cpp
    ../ntp/ntp_synchronizer.cpp
)

# This target installs the module headers and the umbrella header.
target_sources(pico_time_time_sync PUBLIC
    FILE_SET HEADERS
    BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PICO_TIME_PUBLIC_INCLUDE_DIR}
    FILES
        include/pico_time/time_sync/time_sync.hpp
        ${PICO_TIME_PUBLIC_INCLUDE_DIR}/pico_time/pico_time.hpp
)

# The firmware build checks the header version against the CMake version.
house_cxx_version_definitions(pico_time_time_sync pico_time PRIVATE)

# PRIVATE keeps these headers off the public build interface. The installed
# package lacks them, so code that compiles in-tree would fail downstream.
target_include_directories(pico_time_time_sync PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core>
)

target_include_directories(pico_time_time_sync PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PICO_TIME_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_time_time_sync PUBLIC cxx_std_20)

# Public headers use core epoch bounds and ntp error and Result types.
target_link_libraries(pico_time_time_sync
    PUBLIC
        pico_logging::core
        pico_time::core
        pico_time::ntp
    PRIVATE
        pico_time_strict
)

# SDK targets cannot be exported. The installed config reapplies their names
# after the consumer initialises its SDK.
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
