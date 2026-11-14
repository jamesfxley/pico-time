house_cxx_add_module_test(pico_time ntp
    SOURCES
        ntp_synchronizer_test.cpp
)

# Tests use core's private wire-format header through an explicit include path.
target_include_directories(pico_time_ntp_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core
)

target_link_libraries(pico_time_ntp_tests PRIVATE pico_time::testing)
