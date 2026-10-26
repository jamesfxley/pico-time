# The umbrella header belongs to no module (CXX-13.1), so its directory is named
# here and the module that installs it refers to this variable rather than
# reaching back up the tree by relative path.
set(PICO_TIME_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(core)
add_subdirectory(ntp)
add_subdirectory(testing)

# core and ntp are the portable half and need no target of their own.
# Everything below is the lwIP transport and the newlib shims, which do.
if(NOT PICO_TIME_BUILD_FIRMWARE)
    message(STATUS
        "pico-time: PICO_TIME_BUILD_FIRMWARE is OFF, building the portable "
        "core only")
    return()
endif()

add_subdirectory(lwipopts)

# AR-5. The variable is the family's, not this library's: an application
# linking pico-time and another networking library has one lwipopts.h and now
# names it once.
pico_family_require_lwipopts_target(pico-time
    FALLBACK pico_time_lwipopts_fallback)

add_subdirectory(time_sync)
