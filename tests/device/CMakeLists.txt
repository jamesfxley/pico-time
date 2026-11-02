pico_family_add_device_test(pico_time_device_tests
    PACKAGE pico_time
    SOURCES main.cpp
    LIBRARIES pico_time::time_sync ${PICO_LWIPOPTS_TARGET})

# newlib sys/features.h:107 enables BSD declarations with _DEFAULT_SOURCE;
# sys/time.h:419-423 guards settimeofday with __BSD_VISIBLE.
target_compile_definitions(pico_time_device_tests PRIVATE _DEFAULT_SOURCE=1)
