# Using pico-time

The README covers what the API is and what a server has to prove. This is the
other half — wiring it into an image, and the four things that catch people.

## Linking it, and the one thing that is not optional

```cmake
pico_family_dependency(pico_time
    REPOSITORY https://github.com/jamesfxley/pico-time.git
    REVISION <full-commit-id>
)
pico_family_make_available(pico_time)

target_link_libraries(my_firmware PRIVATE pico_time::time_sync)
```

A firmware configure **fails** unless you point `PICO_LWIPOPTS_TARGET` at
a target carrying your `lwipopts.h`:

```sh
cmake -B build-firmware -DPICO_LWIPOPTS_TARGET=my_app_lwipopts
```

That refusal is deliberate, and it is not a convenience check. lwIP requires
the application to supply `lwipopts.h`, and the SDK compiles the whole lwIP
core into whichever target links its INTERFACE libraries — so the configuration
is not this library's view of lwIP, it is a complete second copy of lwIP inside
`libpico_time_time_sync.a`. Let it default silently and you link two lwIPs built
against different options, whose struct layouts and memory-pool tables disagree;
which one the linker resolves depends on archive order, and the failure is
silent memory corruption a long way from the cause.

Use `pico_time_lwipopts_fallback` only if you have no configuration of your
own:

```sh
cmake -B build-firmware -DPICO_LWIPOPTS_TARGET=pico_time_lwipopts_fallback
```

## Synchronizing

Wi-Fi must be up first — this library does not bring the link up and will not
wait for it.

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

`synchronize_ntp()` blocks, runs up to three DNS-plus-NTP exchanges each bounded
by the timeout, and succeeds immediately if the clock is already set. Every
entry point is `[[nodiscard]]` because the return value is the only failure
channel: the target builds without exceptions. That value carries an `NtpError`
rather than a bare bool, so a caller can tell a server that refused its own
reply from a link that went quiet, and `retryable()` says which failures clear
on their own.

Nothing else in your image needs to know any of this happened. The newlib shims
mean `gettimeofday()`, `time()` and `std::chrono::system_clock::now()` pick up
the synchronized clock, so logging timestamps and HTTP `Date` headers follow
automatically.

## Re-synchronizing, because the offset drifts

There is no battery-backed RTC. The wall clock is a signed offset between the
Unix epoch and `time_us_64()`, so it drifts with the crystal — order of seconds
per day. Call `synchronize_ntp()` periodically if long-run accuracy matters; a
few times a day is ample for a log timestamp, and more often if you are
correlating against another machine.

Before the first successful sync, timestamps count up from boot rather than
being wrong in an interesting way. Check `is_time_synchronized()` before
treating a timestamp as a wall-clock time.

## The plausibility window expires, and that is the design

Replies are accepted only inside a ten-year window, `MIN_EPOCH_SECONDS` to
`MAX_EPOCH_SECONDS`. The width is load-bearing twice over: it bounds how far a
hostile server can move your clock, and it is what tells the client which NTP
era a timestamp belongs to without already knowing the time — which is what
carries it across the 2036-02-07 era rollover.

The consequence to plan for: **a build stops being able to synchronize.** On the
current floor, an image refuses every time from 2035-01-01T12:00:00Z onwards and
cannot set its clock again until it is rebuilt on a later floor. For a device
with a long field life, that is a scheduled rebuild, not a bug report.

## Reading the clock from an interrupt

Safe. The 64-bit offset is published through `pico_util::Published`: a ring of
four slots written in turn, stamped with a monotonic counter that never
repeats. A reader takes the counter, reads the slot it names, and re-reads the
counter to confirm no store intervened, retrying up to three times before
reporting no value. The counter is what makes that check conclusive — an
alternating index could return to the value the reader first saw with a write
to that very slot in between, and the check would pass over a torn read.

Every atomic involved is at most 32 bits and performs only loads and stores, which are
genuinely lock-free on both targets; `pico_util` holds that with its own
`static_assert`. A reader never spins on a writer and never disables
interrupts, so calling it from the lwIP background IRQ context is fine.

## Setting the clock from somewhere else

If you have an epoch time from an RTC module or a server response, apply it
directly:

<!-- example: firmware/guide_apply_time -->
```cpp
if (const auto applied = pico_time::apply_wall_clock_time(seconds, microseconds);
    !applied.ok()) {
    // MICROSECONDS_INVALID above 999999, EPOCH_OUT_OF_RANGE outside the window
    PICO_LOG_WARN << pico_time::describe(applied.error());
}
```

It fails and leaves the clock untouched rather than applying a value it cannot
vouch for, and says which bound was broken. The same window applies, for the
same reasons.

Call it from mainline context, from one caller, and at most once per second.
`synchronize_ntp()` satisfies that on its own — it stores at most once per
boot, since once synchronized it returns before any work (NR-4) — but this
entry point and the `settimeofday()` shim over it do not, and nothing enforces
it. The publication ring above is sized against those terms. A single writer
storing in a tight loop that laps the ring inside every attempt of a concurrent
read makes that read fail rather than return a torn value -- `gettimeofday()`
returns -1 with `errno` set to `EAGAIN`, and `time()` returns `(time_t)-1`.
Two writers at once, on two cores or one in an interrupt, are not detected: a
read can return a time made of both.

`REVISION` takes the full commit ID of the release tag;
`git rev-parse v0.1.0^{commit}` prints it.
