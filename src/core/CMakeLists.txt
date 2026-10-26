# The portable core: the SNTP wire format and the wall-clock arithmetic and
# publication protocol. No lwIP, no Pico SDK, no logger -- which is what lets
# the validation rules and the clock be host-tested and fuzzed rather than
# argued about. It is built in both trees; the firmware library links it.
add_library(pico_time_core STATIC
    ntp_message.cpp
    wall_clock.cpp
)
add_library(pico_time::core ALIAS pico_time_core)

target_sources(pico_time_core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_time/core/epoch.hpp
)

# ntp_message.hpp and wall_clock.hpp are implementation detail (CXX-13.3): they
# sit beside the sources that use them and are in no FILE_SET, so they are not
# installed. PRIVATE rather than PUBLIC for exactly that reason -- on the public
# build interface they would be includable by an in-tree consumer and absent
# from the installed package, which compiles here and fails downstream.
target_include_directories(pico_time_core
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_time_core PUBLIC cxx_std_20)
# pico_util::core is PRIVATE: wall_clock.hpp is an internal header of this
# module -- it is not installed and no public header includes it -- so
# pico_util::Published reaches nothing outside this target.
target_link_libraries(pico_time_core
    PRIVATE pico_time_strict pico_util::core)
