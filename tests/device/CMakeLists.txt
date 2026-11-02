pico_family_add_device_test(pico_time_device_tests
    PACKAGE pico_time
    SOURCES main.cpp
    LIBRARIES pico_time::time_sync ${PICO_LWIPOPTS_TARGET})
