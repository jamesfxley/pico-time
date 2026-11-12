# Host tests drive slot exhaustion and abandoned lookup release (AR-6).
add_library(pico_time_ntp STATIC
    ntp_synchronizer.cpp
)
add_library(pico_time::ntp ALIAS pico_time_ntp)

target_sources(pico_time_ntp PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_time/ntp/ntp_error.hpp
        include/pico_time/ntp/ntp_transport.hpp
        include/pico_time/ntp/ntp_synchronizer.hpp
)

# PRIVATE keeps these headers off the public build interface. The installed
# package lacks them, so code that compiles in-tree would fail downstream.
target_include_directories(pico_time_ntp
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core>
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_time_ntp PUBLIC cxx_std_20)

# Public headers expose Result and the epoch bounds, so their targets propagate.
target_link_libraries(pico_time_ntp
    PUBLIC
        pico_time::core
        pico_util::core
    PRIVATE
        pico_time_strict
)
