# Using pico-time

The [README](../README.md) introduces the library. This guide covers firmware
configuration, clock reads and direct time setting.

`pico_time/time_sync/time_sync.hpp` provides the firmware API, epoch bounds and
errors. Its three entry points, `synchronize_ntp()`, `is_time_synchronized()`
and `apply_wall_clock_time()`, are `[[nodiscard]]`. Return values are the only
failure channel on the exception-free target.

## Linking

```cmake
pico_family_dependency(pico_time
    REPOSITORY https://github.com/jamesfxley/pico-time.git
    REVISION <full-commit-id>
)
pico_family_make_available(pico_time)

target_link_libraries(my_firmware PRIVATE pico_time::time_sync)
```

Firmware configuration fails unless `PICO_LWIPOPTS_TARGET` names the target
carrying your application's `lwipopts.h`:

```sh
cmake -B build-firmware -DPICO_LWIPOPTS_TARGET=my_app_lwipopts
```

The [architecture](architecture.md#lwip-configuration) explains why the options
have no default. Use the fallback only if you have no configuration of your own:

```sh
cmake -B build-firmware -DPICO_LWIPOPTS_TARGET=pico_time_lwipopts_fallback
```

## Synchronisation

Wi-Fi must be connected first. The library neither connects nor waits for it.

<!-- example: firmware/guide_synchronize -->
```cpp
#include <pico_time/time_sync/time_sync.hpp>

if (const auto synced = pico_time::synchronize_ntp(); !synced.ok()) {
    PICO_LOG_WARN << "no time yet: " << pico_time::describe(synced.error());
    if (pico_time::retryable(synced.error())) {
        // Slot exhaustion, a timeout, or a DNS, socket or send failure: try
        // again later.
    }
}
```

`synchronize_ntp()` blocks for up to three exchanges, each bounded by the
timeout, with one-second delays between them. If all three attempts time out,
the call blocks for about three timeouts plus two seconds. The default timeout
is ten seconds. The call returns `pico_util::Result<void, NtpError>` and
succeeds immediately if the clock is already set. Invalid timeouts and slot
exhaustion return without network traffic. `retryable()` classifies transient
failures.
`is_time_synchronized()` reports whether an NTP or manual set has succeeded.

The newlib hooks make `gettimeofday()`, `time()` and
`std::chrono::system_clock::now()` read the clock automatically. Logging
timestamps and HTTP `Date` headers use the same time.

## Clock drift

The SDK counter runs from the board crystal, so the wall clock drifts between
sets. Repeat calls to `synchronize_ntp()` do not correct drift: once the clock
is set, the function returns without an exchange (NR-4). A correction needs a
time obtained elsewhere and passed to the
[direct setter](#setting-the-clock-from-somewhere-else).

Before the first set, timestamps count from boot.
Check `is_time_synchronized()` before treating them as epoch times.

## Epoch window

The accepted interval is `[MIN_EPOCH_SECONDS, MAX_EPOCH_SECONDS)`.
The [architecture](architecture.md#epoch-window) explains its width and era
selection. An image with the current floor rejects times from
2035-01-01T12:00:00Z onwards. A device used beyond that instant needs a rebuild
with a later floor (FR-8, NR-5).

## Reading the clock from an interrupt

Interrupt reads use a bounded slot-ring publication protocol. The
[architecture](architecture.md#the-wall-clock) describes its counter and
lock-free atomics. Readers neither spin on a writer nor disable interrupts.
An unvalidated read makes `gettimeofday()` return -1 with `errno` set to
`EAGAIN`, and `time()` return `(time_t)-1` (FR-6).

## Setting the clock from somewhere else

`apply_wall_clock_time(seconds, microseconds)` applies a Unix epoch time
obtained from an RTC module or another server. It returns
`pico_util::Result<void, WallClockError>`:

<!-- example: firmware/guide_apply_time -->
```cpp
if (const auto applied = pico_time::apply_wall_clock_time(seconds, microseconds);
    !applied.ok()) {
    // MICROSECONDS_INVALID above 999999, EPOCH_OUT_OF_RANGE outside the window
    PICO_LOG_WARN << pico_time::describe(applied.error());
}
```

Microseconds above 999999 or seconds outside the epoch window leave the clock
unchanged and return `MICROSECONDS_INVALID` or `EPOCH_OUT_OF_RANGE` (AR-3).

`settimeofday()` is the standard C entry point in `<sys/time.h>`. Newlib exposes
its declaration when `_DEFAULT_SOURCE` is enabled before system headers, for
example as an application compile definition. The library overrides the SDK
setter and delegates to `_settimeofday()`. Both reject a null time pointer,
invalid fields or an out-of-window epoch with -1 and `errno` set to `EINVAL`.
The clock stays unchanged on failure. A valid call returns 0 and sets the
shared clock. The timezone pointer is ignored (AR-4).

A consumer must not define `settimeofday()`, `_settimeofday()` or
`_gettimeofday()`. pico-time supplies these hooks; duplicate definitions fail
the link.

Call setters from one mainline writer, at most once per second.
`synchronize_ntp()` stores at most once per boot (NR-4). Direct setters have no
rate or context check. Excess stores can exhaust a read's validation attempts.
Concurrent writers are not detected and can produce a torn value reported as
valid (FR-6).

`REVISION` takes the full commit ID of the release tag.
`git rev-parse v0.2.0^{commit}` prints it.
