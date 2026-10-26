# Test support in the shipped package, not under tests/: a downstream project
# driving NtpSynchronizer in its own tests needs the same double this
# repository's tests use, and a copy in each tree is free to drift from the
# contract it stands in for (EN-04.1).
#
# INTERFACE: the fake is header-only, and its only obligation is the contract in
# its own documentation.
add_library(pico_time_testing INTERFACE)
add_library(pico_time::testing ALIAS pico_time_testing)

target_sources(pico_time_testing INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_time/testing/fake_ntp_transport.hpp
)

target_include_directories(pico_time_testing INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_time_testing INTERFACE cxx_std_20)
target_link_libraries(pico_time_testing INTERFACE pico_time::ntp)
