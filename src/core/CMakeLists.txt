# Portable wire validation and clock publication build in both trees.
add_library(pico_time_core STATIC
    ntp_message.cpp
    wall_clock.cpp
)
add_library(pico_time::core ALIAS pico_time_core)

target_sources(pico_time_core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_time/core/epoch.hpp
)

# PRIVATE keeps these headers off the public build interface. The installed
# package lacks them, so code that compiles in-tree would fail downstream.
target_include_directories(pico_time_core
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_time_core PUBLIC cxx_std_20)
# Published is used only by the private wall_clock.hpp header.
target_link_libraries(pico_time_core
    PRIVATE pico_time_strict pico_util::core)
