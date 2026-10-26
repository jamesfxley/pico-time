# pico-time

[![CI](https://github.com/jamesfxley/pico-time/actions/workflows/ci.yml/badge.svg)](https://github.com/jamesfxley/pico-time/actions/workflows/ci.yml)

`pico-time` keeps the wall clock of a Raspberry Pi Pico 2 W in sync.
It provides:

- `pico_time::synchronize_ntp()` — a small NTP client (lwIP raw UDP + DNS)
  that queries `pool.ntp.org`
- newlib `_gettimeofday`/`_settimeofday` shims so `std::chrono::system_clock`
  and `time()` reflect the synchronized epoch (used by
  [`pico-logging`](https://github.com/jamesfxley/pico-logging) timestamps)

The **portable core** — the SNTP wire format, and the wall-clock arithmetic and
its publication protocol — builds and is tested on a host. Only the lwIP client
and the newlib shims need the SDK. Build infrastructure is pinned at `infra/`: pico-tooling refers to House and
the engineering standard. `house.toml` selects the family and package modes.

`docs/technical-specification.md` is normative: what the library guarantees is
what carries a `FR`/`NR`/`VR`/`AR` requirement ID there.

## Quick start

```cmake
include(FetchContent)
FetchContent_Declare(pico_time
    GIT_REPOSITORY https://github.com/jamesfxley/pico-time.git
    GIT_TAG v0.1.0)  # pin the release you build against
FetchContent_MakeAvailable(pico_time)

target_link_libraries(app PRIVATE pico_time::time_sync)
```

lwIP requires the application to provide `lwipopts.h`, and
`PICO_LWIPOPTS_TARGET` **must** name the INTERFACE target carrying yours —
configuration fails without it. There is deliberately no default: the Pico SDK
compiles lwIP into whichever target links its INTERFACE libraries, so a
fallback would build a *second* lwIP against a configuration your application
does not use, whose struct layouts and memory-pool tables disagree with the one
the rest of your stack uses.

To build this repository standalone, pass
`-DPICO_LWIPOPTS_TARGET=pico_time_lwipopts_fallback`.

<!-- example: firmware/readme_synchronize -->
```cpp
#include <pico_time/time_sync/time_sync.hpp>

// After cyw43_arch init + Wi-Fi is connected:
if (pico_time::synchronize_ntp().ok()) { /* wall clock is now correct */ }
```

## Layout

- `src/core/` — `pico_time::core`: the wall-clock offset with its publication
  protocol, and the NTP wire format with every validation rule. It depends on
  nothing from the SDK, which is what lets it be host-tested and fuzzed.
- `src/ntp/` — `pico_time::ntp`: the attempt loop, the slot pool an
  uncancellable lookup outlives, and the `NtpError` vocabulary with
  `retryable()`. It reaches the network only through `INtpTransport`, so it is
  host-testable in full.
- `src/testing/` — `pico_time::testing`: `FakeNtpTransport`, the scripted
  transport a consumer's own tests drive the policy against. Exported, and
  header-only.
- `src/time_sync/` — `pico_time::time_sync`: the lwIP glue that resolves and
  exchanges, and the newlib `_gettimeofday`/`_settimeofday` shims that put the
  synchronized clock behind `std::chrono::system_clock`. Firmware only.
- `src/lwipopts/` — a fallback lwIP configuration, reachable only by naming
  `pico_time_lwipopts_fallback` in `PICO_LWIPOPTS_TARGET`. There is no
  silent default: lwIP configuration belongs to the application.
- `src/include/pico_time/pico_time.hpp` — the umbrella header and the version
  constants, belonging to no module (`CXX-13.1`).
- `docs/` — the normative specification (requirement IDs `FR/NR/VR/AR`),
  [architecture](docs/architecture.md) and a
  [usage guide](docs/usage-guide.md).

## Building & tests

The devcontainer builds the pinned pico-tooling image. Initialize recursive
submodules before running the Python entry point:

```sh
git submodule update --init --recursive
python3 dev.py check --list
python3 dev.py build --tree host
python3 dev.py check --ci
```

For the full local check, select the workflow's declared inputs as described in
[the orientation](docs/orientation.md). Firmware needs `PICO_SDK_PATH` and the
explicit lwIP options target. Host, sanitizer, fuzz and firmware trees each
retain build and lint evidence. The fuzz tree runs a bounded seed-corpus test.
The installed firmware consumer links the relocated package after initializing
its own SDK; host installation is refused.

`examples/ntp_sync/` is a flashable image for the on-device validation that the
host suite cannot reach (`FR-2`, `FR-7`, `NR-3`, `NR-4`).

## API

Everything lives in `pico_time/time_sync/time_sync.hpp`, which pulls in the
epoch bounds from `pico_time/core/epoch.hpp`:

All three are `[[nodiscard]]`: the return value is the only failure channel,
since the target builds without exceptions.

- `synchronize_ntp(timeout)` — blocking; requires Wi-Fi to be up. Runs up to
  three DNS + NTP exchanges (each bounded by `timeout`, default 10 s) and
  succeeds once the wall clock has been set. Succeeds immediately if the clock
  is already synchronized. Returns `pico_util::Result<void, NtpError>`:
  `NtpError` names every failure the client can distinguish — a refused reply
  says which check refused it — and `retryable(error)` says whether trying
  again could succeed without anything else changing. Fails without network
  traffic with `INVALID_TIMEOUT` if `timeout` is not positive or exceeds
  `MAX_SYNC_TIMEOUT` (1 h), and with `NO_ATTEMPT_SLOT` — which is retryable —
  when every attempt slot is still held by a lookup that has not landed.
- `is_time_synchronized()` — whether the wall clock has been set at least
  once (by NTP or manually).
- `apply_wall_clock_time(seconds, microseconds)` — set the wall clock
  directly from an epoch time you obtained elsewhere (e.g. an RTC module or
  a server response); this is what the NTP client calls internally. Returns
  `pico_util::Result<void, WallClockError>`, leaving the clock untouched, with
  `MICROSECONDS_INVALID` above 999999 and `EPOCH_OUT_OF_RANGE` outside
  `[MIN_EPOCH_SECONDS, MAX_EPOCH_SECONDS)` — telling the two apart is what lets
  a caller who passed microseconds into the seconds argument find its mistake.

## What a server has to prove

An NTP reply is untrusted input, and every reply is validated before it is
applied (`VR-1`–`VR-6`). It must be a server-mode message, not carry the leap
alarm, sit at a usable stratum, echo the 64-bit nonce this client sent in that
request, carry a non-zero transmit timestamp, and land inside a plausibility
window. A reply failing any of these is refused rather than applied.

The nonce is what makes a captured reply useless when replayed later — in
unauthenticated SNTP it is the only evidence a reply belongs to the request in
flight. It is also checked first, ahead of everything else, so a datagram that
has not proved it belongs to this attempt is dropped rather than allowed to
fail it: neither a forged nor a replayed packet can end an attempt that would
otherwise have succeeded (`VR-7`).

The plausibility window is ten years wide, `MIN_EPOCH_SECONDS` to
`MAX_EPOCH_SECONDS`, and its width does real work — it bounds how far a hostile
server can move your clock, and it is what tells the client which NTP era a
timestamp belongs to without already knowing the time. **It also expires.** On
the current 2025-01-01 floor, a build refuses every time from
2035-01-01T12:00:00Z onwards and cannot synchronize after that instant until it
is rebuilt on a later floor. That is also what will carry the client across the
2036-02-07 era rollover (`FR-8`, `NR-5`).

Installed `pico_time::testing` serves on-target consumer tests. Host tests
obtain the fake in-tree through `FetchContent`.

## How it works

The board has no battery-backed RTC, so the "wall clock" is a signed offset
between the Unix epoch and the SDK's monotonic microsecond counter
(`time_us_64()`). Synchronizing just stores that offset; reads add it back.
Consequences:

- Time never jumps backwards mid-boot unless you re-synchronize; before the
  first sync, timestamps count up from boot (clamped to stay non-negative).
- The offset drifts with the crystal (order of seconds per day) — call
  `synchronize_ntp()` periodically if long-run accuracy matters.
- The offset is 64 bits on a 32-bit machine, published through a four-slot ring
  stamped by a monotonic counter. A reader makes up to three validated
  attempts; exhaustion makes `_gettimeofday` fail with `EAGAIN` (FR-6).
  Every atomic is 32 bits and lock-free on this target, checked by
  `static_assert`. The reader never spins or disables interrupts, including
  when called from the lwIP background IRQ context.

The newlib shims mean nothing else needs to know any of this:
`gettimeofday()`, `time()`, `std::chrono::system_clock::now()`, and anything
built on them (logging timestamps, HTTP `Date` headers) pick up the
synchronized clock automatically.

## Documentation

- [Documentation index](docs/README.md) — what each document is for
- [Technical specification](docs/technical-specification.md) — normative;
  requirement IDs `FR/NR/VR/AR`, the gates and the traceability matrix
- [Architecture](docs/architecture.md) — including the epoch window, the single
  server, the stepped offset, and what this is deliberately not — the host/firmware split, the wall
  clock's publication protocol, the attempt slot pool, lwIP configuration
- [Usage guide](docs/usage-guide.md) — linking it, synchronizing and
  re-synchronizing, the plausibility window, reading the clock from an ISR
- [Standing deviations](docs/deviations.md) — scope-wide rule suppressions and
  their review dates
- [Releases](https://github.com/jamesfxley/pico-time/releases) — what changed per
  release, and what to change in your code when an API broke
- [Contributing](CONTRIBUTING.md)
  — how a change is made, under the
  [engineering standard](https://github.com/jamesfxley/engineering-standard)

## Releases

Versioning is semver: breaking changes only at a major version. The notes on
each [release](https://github.com/jamesfxley/pico-time/releases) record what
changed and, for a break, what to change in your own code (`EN-06.3`).

## AI-assisted development

This project is developed with AI assistance. Design decisions, review and
acceptance are human; an AI agent writes and refactors code under direction and
against a written engineering standard. Every change passes the repository's own
gates — build, tests, formatter, linter, deviation counts — before it lands.

This section is disclosure, not a warranty. The terms in [LICENSE](LICENSE)
apply unchanged, including its disclaimer of warranties.
