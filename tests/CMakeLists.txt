# Module tests are added after their library targets.
#
# The firmware glue needs a board. lwipopts contains no code.
# Host tests cover the portable core and NTP policy.

add_subdirectory(core)
add_subdirectory(ntp)
