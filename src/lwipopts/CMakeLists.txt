# The fallback is opt-in. docs/architecture.md owns the configuration rationale.
add_library(pico_time_lwipopts_fallback INTERFACE)
add_library(pico_time::lwipopts_fallback ALIAS pico_time_lwipopts_fallback)

target_sources(pico_time_lwipopts_fallback INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/lwipopts.h
)

# BUILD_INTERFACE keeps producer paths out of installed include directories.
target_include_directories(pico_time_lwipopts_fallback INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
