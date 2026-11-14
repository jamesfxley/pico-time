# pico-time

This guide covers repository builds, layout and local constraints.
The [README](../README.md) introduces the library; the
[architecture](architecture.md) describes its implementation.
`EN-` and `CXX-` rules resolve at `infra/house/standard/`.

## Commands

`python3 dev.py check` runs the local gates. `--ci` enforces declared CI
allowances, `--list` lists gates, and `--all` adds online checks.
Initialise recursive submodules first. The full workflow environment is:

```sh
export HOUSE_MATRIX_SANITIZE=ON
export HOUSE_MATRIX_BOARD=pico2_w
export HOUSE_MATRIX_LWIPOPTS=pico_time_lwipopts_fallback
export HOUSE_MATRIX_SSID=placeholder-ssid
export HOUSE_MATRIX_PASSWORD=placeholder-password
python3 dev.py check --ci
```

Set `PICO_SDK_PATH` to your SDK checkout. Firmware needs RP2350 and CYW43.
The `pico_w` and `pico2` consumer cases check configuration failure.
Firmware configuration also fails without `PICO_LWIPOPTS_TARGET`. The workflow
selects the standalone fallback explicitly. Placeholder credentials retain the
Wi-Fi connection path in the footprint image. Do not commit real credentials.

`python3 dev.py build` clean-builds and tests enabled trees. `--tree host`,
`--tree firmware` and `--tree fuzz` select one. Workflow lint preparation runs
that build command. Firmware preparation also runs
`python3 dev.py gate build-warnings`. Build matrices include Debug and Release.
The `consumer` gate builds a relocated installed-package consumer and checks
expected configuration failures. Firmware consumers are not run.

`python3 dev.py format --check`, `python3 dev.py lint` and
`python3 dev.py gate api-docs` run individual check groups.

`house.toml` selects `infra/family.toml`. `pico.toml` owns footprint and symbol
budgets. `python3 dev.py sync` writes managed tool files and workflows;
`python3 -m infra.pico_tooling sync` generates the CMake bootstrap.
The root `CMakeLists.txt` declares product pins. There are no shell entry points
or tooling symlinks. Flashing and device validation require separate approval.

## Layout

Module headers live with their sources. Only headers in a `FILE_SET` are
installed (CXX-13.1).

- `src/include/pico_time/pico_time.hpp`: the umbrella and version constants.
- `src/time_sync/include/pico_time/time_sync/time_sync.hpp`: the firmware API,
  including epoch bounds and errors. Fallible calls return `pico_util::Result`
  with library-owned errors (CXX-07.1).
- `src/core/include/pico_time/core/epoch.hpp`: the window shared by wire parsing
  and clock validation.
- `src/core/ntp_message.{hpp,cpp}`: portable SNTP wire parsing and validation.
- `src/core/wall_clock.{hpp,cpp}`: portable offset arithmetic and publication.
  Both core implementation headers are private and excluded from installation.
  A public build interface would let code that compiles in-tree fail against
  the installed package.
- `src/ntp/include/pico_time/ntp/`: errors, the transport interface and the
  portable attempt loop with its slot pool.
- `src/testing/include/pico_time/testing/fake_ntp_transport.hpp`: the scripted
  transport, installed for firmware tests. Host consumers use in-tree targets.
- `src/time_sync/lwip_ntp_transport.{hpp,cpp}`: the lwIP adapter.
  `ntp_client.cpp` composes the client and applies its result.
  `time_keeper.cpp` defines newlib hooks.
- `src/lwipopts/`: the explicit `pico_time_lwipopts_fallback` target.
- `tests/core/` and `tests/ntp/`: host tests mirroring their modules. The core
  tests also check the umbrella and version constants (CXX-13.2).
- `tests/firmware/`: compile-only document examples and a linked setter probe.
  The `lwipopts-required` consumer case checks firmware configuration (AR-5).
- `tests/device/`: board clock tests and the lwIP loopback fixture.
- `examples/ntp_sync/`: the Wi-Fi validation image.

The public installed modules are `pico_time::time_sync`, `pico_time::core`,
`pico_time::ntp` and `pico_time::testing`. Installation is firmware-only.
Host tests use the portable modules in-tree.

## Conventions

### House policy

Shared policy is routed through the [pinned family guides](../infra/agents/AGENTS.md).

### Repo specifics

- The [module split](architecture.md#host-and-firmware-modules) places parsing,
  clock arithmetic and attempt policy in portable modules. `src/time_sync/`
  contains firmware glue. Device tests exercise its clock and loopback paths;
  structural properties remain review-only in the traceability matrix.
- The [slot pool](architecture.md#the-ntp-exchange) outlives uncancellable
  resolutions. Host tests drive late callbacks through `FakeNtpTransport`.
- Clock behaviour must hold as specified: reads follow the counter between
  sets, pre-sync reads count from boot and results stay non-negative.
  The [wall clock](architecture.md#the-wall-clock) describes publication and
  the reason for using 32-bit atomics.
- The firmware bootstrap declares `PLATFORM rp2350 CYW43`. A `pico_w` configure
  fails (AR-7). The RP2040 limitation is tracked separately (FW-1).
- The [usage guide](usage-guide.md#synchronisation) covers the blocking NTP
  call. Its `pico_util::Result<void, NtpError>` has an explicit `operator bool`.
- Replies are validated before use (VR-1–VR-6). A zero transmit seconds field
  must be rejected before era rebasing can place it in 2036.
- No lwIP callback may log. The [architecture](architecture.md#the-ntp-exchange)
  explains the mainline logging requirement.
- The newlib hooks expose the clock through standard functions. Callers should
  not need an additional opt-in to read it.
- Host tests cover the core and policy. Claims about the lwIP adapter, SDK
  counter or newlib link need device evidence from `tests/device/` or the Wi-Fi
  example. The specification distinguishes their coverage.
- Firmware `-Werror` applies per source, since SDK INTERFACE libraries add SDK
  sources to library targets. `house_cxx_werror_sources(...)` registers owned
  sources. The build-log check catches missed registrations. Do not replace
  per-source flags with target-wide flags or build-log matching alone.
- Strict flags use toolchain-derived `FREESTANDING` and `LANGUAGES CXX`.
  Freestanding mode omits hardening that needs a hosted libc or dynamic loader.
  C++ scope excludes diagnostics for SDK-owned C sources.
- `misc-include-cleaner` names newlib internals for `timeval` and `suseconds_t`.
  The source uses POSIX `<sys/time.h>` with site suppressions. Public SDK
  headers `hardware/timer.h` and `pico/types.h` are included directly.
- Lint needs both compile databases to cover firmware-only files. The
  `compile-db` gate checks source coverage.
- The [lwIP configuration rationale](architecture.md#lwip-configuration)
  explains the explicit options target (CXX-10.3).
- `misc-include-cleaner` does not attribute option macros to `<lwip/opt.h>`.
  The option assertion has a site suppression, and the header has a trailing
  `IWYU pragma: keep`. That pragma must stay on the include line. Testing
  `NO_SYS` against zero avoids the preprocessed `1 == 1` diagnostic.
- Suppression placement follows
  [EN-00.4](../infra/house/standard/engineering/00-conformance.md). A justification
  must sit directly above its `NOLINT`, and every comment line must fit within
  80 columns. Reflowing into `NOLINTNEXTLINE` can separate the justification
  from its suppression.
- `NtpError` reaches the caller, and `retryable()` classifies transient errors
  (AR-6). `NtpReject` remains private and maps to public errors through
  `from_reject()`. A rejected reply ends one attempt; later attempts can succeed.
  DNS does not guarantee a different pool server on each lookup.
