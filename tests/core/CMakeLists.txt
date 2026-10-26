house_cxx_add_module_test(pico_time core
    SOURCES
        ntp_message_test.cpp
        pico_time_test.cpp
        wall_clock_test.cpp
)

# A test reaches its module's implementation-detail headers, which are not on
# any public interface and are never installed (CXX-13.3). Naming them here is
# what keeps them off pico_time::core's PUBLIC build interface, where an in-tree
# consumer could include a header the installed package does not ship.
# src/include carries the umbrella, which belongs to no module and is otherwise
# exported only by the firmware-only time_sync target.
target_include_directories(pico_time_core_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/time_sync/include
)

# time_sync.hpp returns a pico_util::Result over pico_time::NtpError, so
# reaching it by include directory alone is no longer enough: the declarations
# have to come from somewhere. Linked rather than named as another directory,
# because that is what a consumer of that header does.
target_link_libraries(pico_time_core_tests PRIVATE pico_time::ntp)

# wall_clock.hpp holds its offset in a pico_util::Published. It is core's
# implementation detail, so core links pico_util PRIVATE and the headers do not
# propagate -- a target reaching that header directly names the dependency for
# itself, exactly as it names the include directory above.
target_link_libraries(pico_time_core_tests PRIVATE pico_util::core)

# pico_time_test.cpp checks the header's advertised version against the one
# CMake holds -- the host-side half of the pin that time_keeper.cpp
# static_asserts in the firmware build.
house_cxx_version_definitions(pico_time_core_tests pico_time PRIVATE)

# The fuzz target reaches core/'s implementation-detail headers, as the test
# above does, so the include directory is set on it after the helper builds it.
house_cxx_add_fuzzer(pico_time_ntp_message_fuzz
    PACKAGE pico_time
    SOURCE ntp_message_fuzz.cpp
    LIBRARIES pico_time::core
)
if(TARGET pico_time_ntp_message_fuzz)
    target_include_directories(pico_time_ntp_message_fuzz PRIVATE
        ${PROJECT_SOURCE_DIR}/src/core)
    # For wall_clock.hpp's pico_util::Published, as for the test target above.
    target_link_libraries(pico_time_ntp_message_fuzz PRIVATE pico_util::core)
endif()

# AR-1 as a build failure: refusal_probe.cpp compiles as written and is part of
# every build; its PROBE=1 copy discards is_time_synchronized()'s result, is
# built only by its test, and passes only on the nodiscard diagnostic. Both are
# compiled and never linked, so the firmware-only definition is not needed.
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
