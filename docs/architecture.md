# pico-time Architecture

How the library is put together. What it guarantees is
[technical-specification.md](technical-specification.md); this file describes
the implementation and is not normative.

## The split that shapes everything

The board has no battery-backed RTC, so the wall clock is an offset from the
SDK's monotonic counter rather than a clock of its own. That makes the whole
library divide cleanly into a part that is pure arithmetic over bytes and
integers, and a part that genuinely needs a target.

| Target | Sources | Needs |
|---|---|---|
| `pico_time::core` | `ntp_message.cpp`, `wall_clock.cpp` | `pico_util`, privately. No lwIP, no Pico SDK, no logger |
| `pico_time::ntp` | `ntp_synchronizer.cpp` | Nothing. Reaches the network only through `INtpTransport` |
| `pico_time::testing` | `fake_ntp_transport.hpp` | Nothing — an `INTERFACE` header target |
| `pico_time::time_sync` | `ntp_client.cpp`, `lwip_ntp_transport.cpp`, `time_keeper.cpp` | lwIP raw API, `cyw43_arch`, newlib |
| `pico_time_lwipopts_fallback` | `lwipopts/include/lwipopts.h` | Nothing — an `INTERFACE` header target |

The core is built in both trees and the firmware library links it. Keeping the
wire format and the publication protocol on the host side is what makes them
testable at their boundaries and fuzzable at all: a validation rule that can
only be exercised against a live NTP server is a rule nobody checks.

A host configure stops after `core`, `ntp` and `testing`.
`PICO_TIME_BUILD_FIRMWARE` is off, the `src/` listfile returns early, and the
firmware-only modules, `time_sync` and the lwIP options target, are never added — so a host build links no SDK sources
and needs no SDK to be present.

## The wall clock

`compute_epoch_offset()` turns an epoch time and a monotonic reading into the
signed microsecond offset between them, or refuses. Refusing is the interesting
half: the seconds argument is bounded so the multiply into microseconds cannot
overflow `int64_t`, which also catches the caller who passes
microseconds-since-epoch where seconds were wanted. `apply_offset()` adds the
offset back and clamps non-negative — defence, not correction, since nothing
that survives `compute_epoch_offset()` can produce a negative result.

`WallClock` publishes the offset through `pico_util::Published` with its
slot-ring strategy, `SLOT_RING_READER_MAY_PREEMPT`. The offset is 64 bits on a
32-bit machine, so a reader can see half of one store and half of the next, and
readers include interrupt context. A seqlock reader spins until the writer
finishes, so an interrupt-context reader that pre-empted its own mainline writer
would spin forever; the ring reader never spins, and retries a bounded number of
times. How the ring works, and why its atomics stay lock-free, is
pico-util's to describe: its architecture's "Two publication strategies".

Four slots, three read attempts, and a writer contract of one store per second
from mainline context only. A synchronization keeps it, since it stores at
most once per boot (NR-4); `apply_wall_clock_time()` and `settimeofday()`
also write and are not rate-limited, so for them the contract is the caller's
(FR-6).

## The NTP exchange

`ntp_message.cpp` is the trust boundary. Everything a server sends arrives
there and nothing downstream re-validates it, so the checks are pure functions
over a 48-byte `std::span` of static extent — no length arithmetic exists to
get wrong. A parse yields an `NtpParse` whose sample is engaged if and only if
the rejection reason is `NONE`, so a caller cannot reach a timestamp by
forgetting to check.

The exchange is split at `INtpTransport`, declared in `ntp_transport.hpp`. The
policy above it — the attempt loop, the slot pool, and the decision about what
a failed attempt means — is `ntp_synchronizer.cpp`, which reaches nothing but
that interface and so is built and tested on the host. Below it,
`lwip_ntp_transport.cpp` is the lwIP raw API adapter, and `ntp_client.cpp` is
composition and logging: it owns the one transport and the one synchronizer,
and holds no policy of its own.

That seam is what makes the pool testable. lwIP is callback-based while
`synchronize_ntp()` is blocking, and the two meet through a fixed pool of
attempt slots:

- The slots are a `std::array<Slot, MAX_REQUEST_ATTEMPTS>` member of
  `NtpSynchronizer`, and the one synchronizer is a function-local static in
  `ntp_client.cpp`. The pool is sized exactly, because an outstanding lookup is
  the only thing that can hold a slot past its attempt.
- A slot is claimed under the cyw43/lwIP lock and released by whichever of the
  caller and the DNS callback finishes last. An lwIP DNS lookup cannot be
  cancelled and will fire after the caller has given up, which is what the
  `abandoned` flag records.
- Completion is a plain flag, published by a callback that runs to completion
  against the mainline on a single core. `VR-8` in the specification carries
  the argument and the configuration it depends on; there is no acquire/release
  pair here, and under a preemptive arrangement there would have to be.

When every slot is still held by a lookup that has not landed on entry,
`synchronize_ntp()` fails with `NtpError::NO_ATTEMPT_SLOT` without touching the
network. That is a transient condition, not a failure of the exchange, and it
clears when those lookups call back — `retryable()` reports it as such.

## Reaching the standard clocks

`time_keeper.cpp` defines `_gettimeofday` and `_settimeofday`, which override
newlib's weak stubs at link time. That is the whole wiring:
`std::chrono::system_clock`, `time()` and `gettimeofday()` pick the
synchronized clock up with nothing further to call. Before the first
synchronization they report time since boot. A read of the offset that cannot
be validated makes `_gettimeofday` fail with `EAGAIN` rather than report a
possibly torn time, and newlib's `time()` passes that on as `(time_t)-1`.

What stays in that file is only what needs the target — the SDK's monotonic
counter and the ABI-mandated syscall names. The arithmetic is next door in
`wall_clock.hpp`, where it can be tested.

## lwIP configuration

`pico_time_lwipopts_fallback` carries a configuration for a consumer that has
none. It is deliberately not a default: the SDK compiles the whole lwIP core
into whichever target links its `INTERFACE` libraries, so this library's view
of `lwipopts.h` is a complete second copy of lwIP inside its own archive. A
consumer that silently got the fallback while building its own lwIP against a
different configuration would link two copies whose struct layouts and memory
pools disagree, resolved by archive order, failing as memory corruption a long
way from the cause.

## Allocation

Nothing in this library's objects reaches the C heap, in any context, and the
gate is `arm-none-eabi-nm -u` over this repository's own objects — not the
whole image, since the SDK compiles its own sources into the same build
directory and `new_delete.cpp.o` references `malloc` by definition. Attempt
state is the static slot pool above; the NTP message is a fixed array; the
clock is two 32-bit atomics per slot. The same `nm` gate rejects soft-float
symbols, which is why the offset arithmetic is integer throughout.

## The epoch window, and why it expires

The plausibility window is `MIN_EPOCH_SECONDS` to `MAX_EPOCH_SECONDS`, ten
Julian years above a 2025-01-01 floor. On that floor a build refuses every
reply timestamped 2035-01-01T12:00:00Z or later and can never synchronize
again.

That is the design working, not a defect. The window's width is what bounds how
far a hostile or broken server can move the clock, and being narrower than an
NTP era is what lets the parser decide which era a timestamp belongs to without
already knowing the time — which is also what carries the client across the
2036-02-07 era rollover. A window wide enough to never expire is no check.

Moving the floor forward is a release's job, and it is the one piece of
maintenance this library cannot avoid. A build older than its own window is
inert rather than wrong: it refuses everything, loudly, rather than accepting a
time it cannot place.

## One server, and no way to name another

The client resolves `pool.ntp.org` and takes the first address DNS returns.
There is no way to name a different server, a fixed address, or a list.

Adding one is small, and the reason it is absent is that the surface it implies
is not: a caller supplying a hostname wants a list, a list wants a selection
policy, and a selection policy wants to know which replies were good — which is
a different library. A consumer needing a specific server today can call
`apply_wall_clock_time()` with a time it obtained itself, which is the seam
that already exists for exactly this.

## The offset is stepped, never slewed

Synchronizing replaces the offset outright. A clock that has drifted a second
jumps a second, which is visible to anything timestamping across the step.

Disciplining — slewing the offset gradually, or measuring drift and correcting
for it between synchronizations — would remove the jump and improve accuracy
between polls. It also turns a stateless offset into a control loop with
filter state, a frequency estimate and a stability criterion, none of which is
testable without a long-running reference. The step is honest and cheap; the
loop would be neither until there was somewhere to validate it.

## What this is not

Server mode, symmetric and broadcast association modes, and NTS or any other
authenticated NTP are out of scope rather than unbuilt. This is an SNTP client
for a board with no RTC: it makes a request and validates the reply. The
anti-replay nonce is the whole of its authentication story, and the
specification says so rather than implying more.

Leap-second scheduling, timezones and local time are likewise absent. The
library produces UTC epoch microseconds; a consumer that needs civil time owns
that conversion, and needs a timezone database this library has no business
carrying.

## Nothing survives a reset

The offset lives in `.bss`. A reset loses it and the clock counts from boot
again until the next successful synchronization.

Persisting it — to flash, or to a battery-backed store the board does not
have — would need a monotonic source that also survives, which the SDK's
counter is not. Without one, a restored offset is an offset from a counter that
restarted at zero: a plausible-looking wrong time, which is worse than a clock
that admits it is unset.
