# The installed fake serves downstream tests of NtpSynchronizer.
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
