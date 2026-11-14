house_cxx_add_module_test(pico_time core
    SOURCES
        ntp_message_test.cpp
        pico_time_test.cpp
        wall_clock_test.cpp
)

# Private core headers and the firmware umbrella need explicit include paths.
target_include_directories(pico_time_core_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/time_sync/include
)

# The API's Result and error declarations propagate through the ntp target.
target_link_libraries(pico_time_core_tests PRIVATE pico_time::ntp)

# Direct users of private wall_clock.hpp also need its Published dependency.
target_link_libraries(pico_time_core_tests PRIVATE pico_util::core)

# Host tests compare the header version to CMake's version.
house_cxx_version_definitions(pico_time_core_tests pico_time PRIVATE)

# The fuzz target also needs core's private headers.
house_cxx_add_fuzzer(pico_time_ntp_message_fuzz
    PACKAGE pico_time
    SOURCE ntp_message_fuzz.cpp
    LIBRARIES pico_time::core
)
if(TARGET pico_time_ntp_message_fuzz)
    target_include_directories(pico_time_ntp_message_fuzz PRIVATE
        ${PROJECT_SOURCE_DIR}/src/core)
    # The private wall_clock.hpp header needs pico_util::Published.
    target_link_libraries(pico_time_ntp_message_fuzz PRIVATE pico_util::core)
endif()

# The default probe compiles. PROBE=1 fails with the nodiscard diagnostic
# (AR-1). Neither object target needs the firmware definition at link time.
add_library(pico_time_refusal_probe OBJECT refusal_probe.cpp)
add_library(pico_time_refuses_discarded_synchronized OBJECT EXCLUDE_FROM_ALL
    refusal_probe.cpp)
target_compile_definitions(pico_time_refuses_discarded_synchronized PRIVATE
    PROBE=1)
foreach(PICO_TIME_PROBE_TARGET pico_time_refusal_probe
        pico_time_refuses_discarded_synchronized)
    target_include_directories(${PICO_TIME_PROBE_TARGET} PRIVATE
        ${PROJECT_SOURCE_DIR}/src/time_sync/include)
    target_link_libraries(${PICO_TIME_PROBE_TARGET} PRIVATE
        pico_time_strict pico_time::ntp)
endforeach()
add_test(NAME pico_time_refuses_discarded_synchronized
    COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=pico_time_refuses_discarded_synchronized "-DEXPECT=nodiscard"
            -P ${PROJECT_SOURCE_DIR}/tests/expect_refusal.cmake)
