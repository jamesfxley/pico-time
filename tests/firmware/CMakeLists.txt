# Document examples are extracted at configure time and compile without linking.
# Linking time_sync would compile SDK and lwIP sources under strict flags.
house_cxx_add_doc_examples(pico_time_firmware_doc_examples PACKAGE pico_time
    GROUP firmware GROUPS firmware
    DOCUMENTS ${PROJECT_SOURCE_DIR}/README.md
              ${PROJECT_SOURCE_DIR}/docs/usage-guide.md
    HARNESS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/docs
    LIBRARIES pico_time::ntp pico_logging::core)
target_include_directories(pico_time_firmware_doc_examples PRIVATE
    ${PROJECT_SOURCE_DIR}/src/time_sync/include)

# Retain the POSIX setter in the linked image for map and symbol checks (AR-4).
add_executable(pico_time_settimeofday_probe settimeofday_probe.cpp)
house_cxx_werror_sources(settimeofday_probe.cpp)
# newlib exposes the BSD/POSIX setter declaration under __BSD_VISIBLE.
target_compile_definitions(pico_time_settimeofday_probe PRIVATE _DEFAULT_SOURCE)
target_link_libraries(pico_time_settimeofday_probe PRIVATE
    pico_time_strict pico_time::time_sync ${PICO_LWIPOPTS_TARGET})
pico_add_extra_outputs(pico_time_settimeofday_probe)
