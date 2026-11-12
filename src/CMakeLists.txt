# The umbrella include directory is shared with the module that installs it.
set(PICO_TIME_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(core)
add_subdirectory(ntp)
add_subdirectory(testing)

# Host builds stop before the SDK-dependent modules.
if(NOT PICO_TIME_BUILD_FIRMWARE)
    message(STATUS
        "pico-time: PICO_TIME_BUILD_FIRMWARE is OFF, building the portable "
        "core only")
    return()
endif()

add_subdirectory(lwipopts)

# Networking libraries share the application lwIP configuration (AR-5).
pico_family_require_lwipopts_target(pico-time
    FALLBACK pico_time_lwipopts_fallback)

add_subdirectory(time_sync)
