# lwIP requires the application to provide lwipopts.h, and the SDK compiles the
# whole lwIP core into whichever target links its INTERFACE libraries -- the
# time_sync target. So the configuration here is not merely this library's view
# of lwIP: it is a complete second copy of lwIP inside
# libpico_time_time_sync.a.
#
# That is why there is no silent default. A consumer that forgot to point this
# at its own configuration would link two lwIPs built against different
# lwipopts, whose struct layouts and memory-pool tables disagree; which copy the
# linker resolves depends on archive order, and the failure is silent memory
# corruption a long way from the cause (CXX-10.3).
add_library(pico_time_lwipopts_fallback INTERFACE)
add_library(pico_time::lwipopts_fallback ALIAS pico_time_lwipopts_fallback)

target_sources(pico_time_lwipopts_fallback INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/lwipopts.h
)

# BUILD_INTERFACE, not a bare path: this target is exported, and install(EXPORT)
# rejects an interface include directory inside the source tree -- it would
# point a consumer at a directory that does not exist on their machine.
target_include_directories(pico_time_lwipopts_fallback INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
