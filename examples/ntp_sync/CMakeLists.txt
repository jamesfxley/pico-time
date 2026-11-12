# The image exercises clock and Wi-Fi behaviour that host tests cannot cover.

# Pass Wi-Fi credentials at configure time, as described in README.md.

add_executable(pico_time_ntp_sync main.cpp)

# Per-source warnings exclude the SDK sources compiled into this target.
house_cxx_werror_sources(main.cpp)

pico_set_program_name(pico_time_ntp_sync "pico_time_ntp_sync")
pico_set_program_version(pico_time_ntp_sync "${PROJECT_VERSION}")

# USB CDC carries the device-validation log.
pico_enable_stdio_usb(pico_time_ntp_sync 1)
pico_enable_stdio_uart(pico_time_ntp_sync 0)

target_compile_definitions(pico_time_ntp_sync PRIVATE
    PICO_TIME_WIFI_SSID="${PICO_TIME_WIFI_SSID}"
    PICO_TIME_WIFI_PASSWORD="${PICO_TIME_WIFI_PASSWORD}"
)

target_link_libraries(pico_time_ntp_sync PRIVATE
    pico_time_strict
    pico_time::time_sync
    pico_logging::core
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
)

# The application owns the lwIP configuration.
target_link_libraries(pico_time_ntp_sync PRIVATE ${PICO_LWIPOPTS_TARGET})

# Generates pico_time_ntp_sync.uf2 (+ .bin/.hex/.elf.map) for flashing.
pico_add_extra_outputs(pico_time_ntp_sync)
