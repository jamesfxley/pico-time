# pico-time architecture

The [technical specification](technical-specification.md) owns the guarantees.
This document describes the implementation and its design reasons.

## Host and firmware modules

The wall clock is an offset from the SDK's monotonic counter. Its arithmetic
and wire parsing need no target services, so they can run in host tests.

| Target | Sources | Needs |
|---|---|---|
| `pico_time::core` | `ntp_message.cpp`, `wall_clock.cpp` | `pico_util`, privately |
| `pico_time::ntp` | `ntp_synchronizer.cpp` | `core`, `pico_util`; network access through `INtpTransport` |
| `pico_time::testing` | `fake_ntp_transport.hpp` | `ntp`; header-only |
| `pico_time::time_sync` | `ntp_client.cpp`, `lwip_ntp_transport.cpp`, `time_keeper.cpp` | lwIP raw API, `cyw43_arch`, newlib |
| `pico_time_lwipopts_fallback` | `lwipopts/include/lwipopts.h` | Header-only |

The core builds in both trees, and the firmware library links it. Host tests
and fuzzing exercise validation at its boundaries without a live NTP server.

A host configure sets `PICO_TIME_BUILD_FIRMWARE` off. The `src/` listfile returns
after adding `core`, `ntp` and `testing`, before any SDK-dependent target.
A host build needs no SDK.

## The wall clock

`compute_epoch_offset()` returns a signed microsecond offset or `nullopt` for
an invalid time. Bounding seconds keeps conversion within `int64_t` and catches
microseconds passed as seconds. `apply_offset()` adds the offset to a monotonic
reading and clamps a negative sum to zero.

`WallClock` uses `pico_util::Published` with
`SLOT_RING_READER_MAY_PREEMPT`. The 64-bit offset needs publication on a 32-bit
machine. An interrupt reader could pre-empt its own writer and deadlock on a
spinning seqlock. The slot ring instead validates reads with bounded retries.
The pinned pico-util source owns the publication algorithm.

A 64-bit atomic would call `__atomic_load_8`, which the SDK implements with a
global spinlock and disabled interrupts. The ring uses 32-bit lock-free
atomics. An unsigned publication counter detects stores between a reader's
samples. An alternating slot index could return to its first value after an
intervening write and allow a torn read to pass validation.

The ring has four slots and three read attempts. The
[usage guide](usage-guide.md#setting-the-clock-from-somewhere-else) states the
writer contract and the NTP store limit (FR-6, NR-4).

## The NTP exchange

`ntp_message.cpp` validates received fields over a fixed 48-byte span. The
static extent removes runtime length arithmetic. A successful `NtpParse` has a
sample; a rejection has none.

The server echoes the request's transmit timestamp in the reply's originate
field (RFC 5905 §8). The client uses a nonce there to distinguish requests
(VR-4). It checks the nonce before content so an unrelated reply cannot end an
attempt (VR-7). This is replay protection, without cryptographic server
authentication.

`INtpTransport` separates policy from network services. `ntp_synchronizer.cpp`
owns attempts, slot lifetimes and errors. `lwip_ntp_transport.cpp` adapts the
lwIP raw API. `ntp_client.cpp` composes them, applies the result and logs it.
Host tests drive the policy through a scripted transport.

The default ten-second timeout allows for DNS and a congested round trip.
One-second retry delays avoid repeating all attempts during one brief
congestion event. The public timeout cap bounds how long a caller blocks.

lwIP callbacks meet the blocking caller through a fixed slot pool:

- `NtpSynchronizer` owns `MAX_REQUEST_ATTEMPTS` slots. Its firmware instance is
  a function-local static, so its slots outlive a call whose lookup calls back
  late.
- Slot claims and releases hold the transport lock. An uncancellable lookup
  retains its slot until both caller and callback finish. `abandoned` records
  that the caller has finished.
- Callbacks hold the transport lock, and the attempt loop reads the completion
  flags under it, so plain flags are published on either core (VR-8).

Callbacks do not log. The SDK's `stdout_serialize_begin` can wait up to
`PICO_STDIO_DEADLOCK_TIMEOUT_MS` while a pre-empted mainline holds `print_mutex`.
Callbacks record an outcome and mainline logging follows.

On entry with every slot held by a pending lookup, `synchronize_ntp()` returns
`NO_ATTEMPT_SLOT` without network traffic. `retryable()` classifies this
condition as transient (AR-6).

`FakeNtpTransport` queues resolutions delivered under its lock until unlock.
It delivers datagrams only during `poll()`. The firmware background IRQ can
arrive at other instructions, so the fake covers only a subset of target
interleavings.

## Standard clock hooks

`time_keeper.cpp` overrides the SDK's weak `_gettimeofday` and `settimeofday`
definitions. `settimeofday` delegates to `_settimeofday`, which validates before
calling `apply_wall_clock_time()`. The timezone argument is ignored.
Standard clock functions use these hooks (AR-4).

Before the first set, reads report time since boot. An unvalidated offset makes
`_gettimeofday` fail with `EAGAIN`; newlib `time()` returns `(time_t)-1`.
The arithmetic remains in portable `wall_clock.hpp` code. Only SDK counter
reads and syscall definitions need the firmware tree.

## lwIP configuration

The SDK compiles lwIP into targets that link its INTERFACE libraries, including
this library's archive. An implicit fallback could put two differently
configured copies in one image. Their struct layouts and memory pools can
disagree, with archive order deciding which definitions link. The resulting
memory corruption can occur far from the configuration error.

`pico_time_lwipopts_fallback` is therefore opt-in. The application names its
configuration through `PICO_LWIPOPTS_TARGET` (AR-5).

## Allocation

The symbol gate checks only this library's objects for C-heap references.
SDK objects in the build directory include runtime allocation references and
are outside that claim. Attempts use a static slot pool, messages use fixed
arrays, and each clock slot contains two 32-bit atomic words (VR-8).

Integer fraction conversion avoids linking the soft-float runtime. The same
symbol gate checks for soft-float references (NR-6).

## Epoch window

The plausibility window runs from `MIN_EPOCH_SECONDS` to the exclusive
`MAX_EPOCH_SECONDS`, ten Julian years above 2025-01-01. It rejects timestamps
from 2035-01-01T12:00:00Z onwards until a release moves the floor (FR-8).

The floor treats earlier dates as unset or forged time.
The window bounds a hostile server's clock adjustment. Its width is less than
one NTP era, so at most one candidate timestamp fits (NR-5; RFC 5905 §6).
The parser tests era 0 and era 1. Moving the window across the 2036 rollover
allows era 1 without requiring a current clock to select the era.

## Server selection

The client resolves `pool.ntp.org` and uses the first returned address.
It has no configurable hostname, fixed address or server list.

A configurable server list would need selection policy and reply assessment.
That is outside this library's scope. A consumer with another source can apply
its time through `apply_wall_clock_time()`.

## Clock adjustment

Setting the clock replaces the offset, so a correction can step time in either
direction. Clock disciplining is outside scope. Slewing and drift correction
would require filter state, a frequency estimate and a stability criterion,
with a long-running reference for testing.

## Protocol scope

Server, symmetric and broadcast modes, NTS and other authenticated NTP are
outside scope. This SNTP client makes requests and validates replies.
The nonce provides replay protection without authenticating the server.

The library produces UTC epoch microseconds. Leap-second scheduling, timezones
and local-time conversion are outside scope. A consumer needing civil time
provides that conversion and its timezone database.

## Reset behaviour

The offset lives in `.bss`. Reset loses it, and the clock reports time since
boot until set again.

Persisting an offset would also need a monotonic source that survives reset.
The SDK counter restarts at zero, so restoring the offset alone would produce
an incorrect time that appears set.
