# The test tree mirrors src/ (CXX-13.2), so src/ is exactly the shippable
# surface: an install, footprint, lint or coverage rule over it needs no test
# exclusion. One directory per module, added after src/ so every
# pico_time::<module> target the tests link already exists.
#
# time_sync and lwipopts are absent on purpose: the first is firmware-only and
# the second carries no code, so neither has a host test binary to build. The
# attempt loop and the slot pool are in ntp/, above the transport seam, and are
# covered here.

add_subdirectory(core)
add_subdirectory(ntp)
