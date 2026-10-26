# Device-validation image. Every traceability row whose criterion is a "device
# run" is established by flashing this and reading the serial log; the host
# suite cannot reach them because they are properties of the SDK counter, the
# radio and the newlib link, not of the portable core.

# Wi-Fi credentials: pass at configure time, e.g.
#   cmake -B build-firmware -DPICO_TIME_WIFI_SSID=myssid -DPICO_TIME_WIFI_PASSWORD=mypassword

add_executable(pico_time_ntp_sync main.cpp)

# Warnings are errors for the code this project owns. Per source rather than on
# the target: the SDK compiles its own sources into this one, and
# pico_time_strict therefore carries WERROR OFF in a firmware build.
house_cxx_werror_sources(main.cpp)

pico_set_program_name(pico_time_ntp_sync "pico_time_ntp_sync")
pico_set_program_version(pico_time_ntp_sync "${PROJECT_VERSION}")

# Route stdio over USB CDC so the family device console can watch the device
# join and sync.
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

# The example is an application, so it is the thing that owns the lwIP
# configuration -- which is exactly the relationship a real consumer has.
target_link_libraries(pico_time_ntp_sync PRIVATE ${PICO_LWIPOPTS_TARGET})

# Generates pico_time_ntp_sync.uf2 (+ .bin/.hex/.elf.map) for flashing.
pico_add_extra_outputs(pico_time_ntp_sync)
