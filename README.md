# pico-time

[![CI](https://github.com/jamesfxley/pico-time/actions/workflows/ci.yml/badge.svg)](https://github.com/jamesfxley/pico-time/actions/workflows/ci.yml)

`pico-time` sets the wall clock of a Raspberry Pi Pico 2 W from `pool.ntp.org`.
It provides a blocking NTP client over lwIP UDP and DNS, with newlib clock hooks
for `std::chrono::system_clock`, `time()` and `gettimeofday()`.
[pico-logging](https://github.com/jamesfxley/pico-logging) uses this clock for
timestamps.

The wire parser, clock arithmetic and NTP attempt loop build and run in host
tests. The lwIP adapter and newlib hooks need the Pico SDK. Build infrastructure
is pinned at `infra/`. `house.toml` selects the family and package modes.

The [technical specification](docs/technical-specification.md) owns the
library's guarantees.

## Quick start

```cmake
include(FetchContent)
FetchContent_Declare(pico_time
    GIT_REPOSITORY https://github.com/jamesfxley/pico-time.git
    GIT_TAG v0.2.0)  # pin the release you build against
FetchContent_MakeAvailable(pico_time)

target_link_libraries(app PRIVATE pico_time::time_sync)
```

Your application provides lwIP's `lwipopts.h`. `PICO_LWIPOPTS_TARGET` must name
the INTERFACE target that carries it. Configuration fails without one. For the
setup and the standalone fallback, see [Linking](docs/usage-guide.md#linking).
The [architecture](docs/architecture.md#lwip-configuration) explains why there
is no default.

<!-- example: firmware/readme_synchronize -->
```cpp
#include <pico_time/time_sync/time_sync.hpp>

// After cyw43_arch initialisation and Wi-Fi connection:
if (pico_time::synchronize_ntp().ok()) { /* wall clock has been set */ }
```

## Layout

- `src/core/`: `pico_time::core`, the portable wire parser and clock arithmetic.
- `src/ntp/`: `pico_time::ntp`, the portable attempt loop, slot pool and errors.
- `src/testing/`: `pico_time::testing`, the header-only `FakeNtpTransport` for
  downstream tests.
- `src/time_sync/`: `pico_time::time_sync`, the firmware API, lwIP adapter and
  newlib clock hooks.
- `src/lwipopts/`: the opt-in fallback lwIP configuration.
- `src/include/pico_time/pico_time.hpp`: the umbrella header and version constants.
- `docs/`: the specification, architecture and usage guide.

## Building & tests

The devcontainer builds the pinned pico-tooling image. Initialise recursive
submodules before running the Python entry point:

```sh
git submodule update --init --recursive
python3 dev.py check --list
python3 dev.py build --tree host
python3 dev.py check --ci
```

For the full local check, set the workflow inputs described in
[Commands](docs/orientation.md#commands). Host, sanitizer, fuzz and firmware
trees retain build and lint evidence. The fuzz tree runs a bounded seed-corpus
test. The installed consumer checks the relocated firmware package. Host
installation fails configuration.

`tests/device/` exercises newlib clocks, invalid POSIX sets, interrupt reads
and unrelated NTP replies. Its lwIP loopback fixture needs no Wi-Fi connection
and covers the adapter. Build it with the device options target; the fallback
options image reports the loopback case as FAIL:

```sh
HOUSE_MATRIX_LWIPOPTS=pico_time_device_lwipopts python3 dev.py build --tree firmware
```

The device options target configures the archive and image with the same
fallback header plus loopback, a local DNS host list and responder resources.
`examples/ntp_sync/` exercises Wi-Fi. Device runs require separate
authorisation; see the [pinned device guide](infra/guides/device.md#device-tests).

## API

The [usage guide](docs/usage-guide.md) covers the firmware API, synchronisation
and direct setters, including bounds and the single-writer contract.

## Reply validation

A reply must have server mode, no leap alarm, a usable stratum, the request
nonce in its originate field, a non-zero transmit timestamp and a time inside
the plausibility window. A nonce mismatch is dropped without ending the attempt.
Other validation failures end the call without a further attempt. If no attempt
succeeds, the call returns the last attempt's error. The
[NTP exchange](docs/architecture.md#the-ntp-exchange) explains the validation
order.

Timestamps are bounded by a ten-year plausibility window. An image cannot set
a time beyond that window; see [Epoch window](docs/architecture.md#epoch-window)
and the [rebuild guidance](docs/usage-guide.md#epoch-window).

Installed `pico_time::testing` serves firmware consumer tests. Host tests obtain
the fake in-tree through `FetchContent`.

## How it works

The wall clock adds a signed offset to the SDK's monotonic counter. Before the
first set, it reports time since boot. Reads follow the counter between sets;
a set can step time in either direction.
See [Clock drift](docs/usage-guide.md#clock-drift) for the limits of repeat NTP
calls.

Interrupt readers use a bounded publication protocol with 32-bit lock-free
atomics. An unvalidated read makes `_gettimeofday` fail with `EAGAIN`.
The [wall-clock architecture](docs/architecture.md#the-wall-clock) describes
the protocol and its writer contract.

The newlib hooks make standard clock reads, logging timestamps and HTTP `Date`
headers use the same wall clock.

## Documentation

- [Documentation index](docs/README.md): document roles and reading routes.
- [Technical specification](docs/technical-specification.md): guarantees,
  gates and traceability.
- [Architecture](docs/architecture.md): implementation and design reasons.
- [Usage guide](docs/usage-guide.md): linking, setting and reading the clock.
- [Standing deviations](docs/deviations.md): approved scope-wide suppressions.
- [Releases](https://github.com/jamesfxley/pico-time/releases): changes and
  migration instructions.
- [Contributing](CONTRIBUTING.md): contribution and verification policy.

## Releases

Versioning follows [EN-06.1 to EN-06.3](infra/house/standard/engineering/06-release-and-versioning.md).
[Release notes](https://github.com/jamesfxley/pico-time/releases) describe
changes and migration steps.

## AI-assisted development

This project is developed with AI assistance. People direct design, review and
acceptance. AI agents write and refactor code against the engineering standard.
Changes pass the repository's gates before they land.

The terms in [LICENSE](LICENSE) apply, including the warranty disclaimer.
