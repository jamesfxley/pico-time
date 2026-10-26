# The NTP client's policy: the attempt loop and the slot pool an uncancellable
# lookup outlives. No lwIP and no Pico SDK -- everything reaches the network
# through INtpTransport -- which is what lets AR-6's slot exhaustion and the
# abandoned-slot release protocol be driven by a host test. Built in both trees;
# the firmware library links it.
add_library(pico_time_ntp STATIC
    ntp_synchronizer.cpp
)
add_library(pico_time::ntp ALIAS pico_time_ntp)

target_sources(pico_time_ntp PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_time/ntp/ntp_error.hpp
        include/pico_time/ntp/ntp_transport.hpp
        include/pico_time/ntp/ntp_synchronizer.hpp
)

# core's ntp_message.hpp is its implementation detail: in no FILE_SET, never
# installed. This module parses replies with it, so it names core's source
# directory PRIVATE -- the same relationship time_sync has, and deliberately not
# core exporting it, which would make the header reachable in-tree and absent
# from the installed package.
target_include_directories(pico_time_ntp
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core>
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_time_ntp PUBLIC cxx_std_20)

# pico_util::core is PUBLIC: synchronize() returns a pico_util::Result, so a
# consumer of ntp_synchronizer.hpp needs that header. core is PUBLIC for the
# epoch header the sample types are stated against.
target_link_libraries(pico_time_ntp
    PUBLIC
        pico_time::core
        pico_util::core
    PRIVATE
        pico_time_strict
)
