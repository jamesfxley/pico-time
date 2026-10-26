house_cxx_add_module_test(pico_time ntp
    SOURCES
        ntp_synchronizer_test.cpp
)

# The synchronizer parses replies with core's implementation-detail header,
# which is on no public interface and is never installed (CXX-13.3). The tests
# build requests with the same header, so they name the directory the same way
# the module does.
target_include_directories(pico_time_ntp_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core
)

target_link_libraries(pico_time_ntp_tests PRIVATE pico_time::testing)
