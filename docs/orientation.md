# pico-time

Keeps the wall clock of a Raspberry Pi Pico 2 W in sync: an lwIP raw
UDP+DNS NTP client plus newlib `_gettimeofday`/`_settimeofday` shims, so
`std::chrono::system_clock` and `time()` reflect the synchronized epoch. Part of
the pico-* family; the clock source behind pico-logging timestamps and HTTP
`Date` headers.

Orientation for anyone working in this repository: how to build and gate it,
how it is laid out, and the repository's invariants and traps.

Engineering rules are not repeated here. `EN-` and `CXX-` IDs cited in this repo
resolve at `infra/house/standard/`.

## Commands

`python3 dev.py check` runs the local gates; `--ci` enforces declared CI
allowances, `--list` lists the gates, and `--all` also requests online checks.
Initialize recursive submodules first. The full workflow environment is:

```sh
export HOUSE_MATRIX_SANITIZE=ON
export HOUSE_MATRIX_BOARD=pico2_w
export HOUSE_MATRIX_LWIPOPTS=pico_time_lwipopts_fallback
export HOUSE_MATRIX_SSID=placeholder-ssid
export HOUSE_MATRIX_PASSWORD=placeholder-password
python3 dev.py check --ci
```

Set `PICO_SDK_PATH` to your SDK checkout. Firmware is RP2350 with CYW43;
`pico_w` and `pico2` are refused by consumer cases. A configure with no
`PICO_LWIPOPTS_TARGET` is refused; the default remains empty. The workflow's
explicit input selects the standalone fallback, never a consumer's implicit
configuration. Placeholder Wi-Fi credentials keep the connect path in the
footprint image. Do not use real credentials in committed configuration.

`python3 dev.py build` clean-builds and tests each enabled tree. Use
`--tree host`, `--tree firmware` or `--tree fuzz` to select one. The generated
workflow runs lint preparation with `python3 dev.py build`, and firmware
preparation adds `python3 dev.py gate build-warnings`. Build matrices include
Debug and Release. `python3 dev.py gate consumer` proves the installed package
and the named refusal cases. Firmware consumers are built, not run.
`python3 dev.py format --check`, `python3 dev.py lint` and
`python3 dev.py gate api-docs` expose the individual groups.

`house.toml` refers to `infra/family.toml`; `pico.toml` owns footprint and symbol
budgets. `python3 dev.py sync` renders the fixed-path tool files and workflow;
`python3 -m infra.pico_tooling sync` generates the CMake bootstrap. Product
pins are declared in the root CMakeLists.txt. There are no shell entry points
or tooling symlinks. Flashing and device validation require separate approval.

## Layout

Four library modules, so the tree takes the multi-module shape (`CXX-13.1`):
a module's headers live with it, and only what is in a `FILE_SET` is installed.

- `src/include/pico_time/pico_time.hpp` — the umbrella, and the version
  constants. Belongs to no module, so it sits above them.
- `src/time_sync/include/pico_time/time_sync/time_sync.hpp` — the sync API:
  `synchronize_ntp()`, `is_time_synchronized()`, `apply_wall_clock_time()`.
  Re-exports the epoch bounds and the error taxonomy, so a caller needs only
  this header. Both fallible entry points return a `pico_util::Result`: the
  carrier is generic, the vocabulary is this library's (`CXX-07.1`).
- `src/core/include/pico_time/core/epoch.hpp` — the wall-clock plausibility
  window. It belongs to the core because the parser and the clock arithmetic
  are what validate against it, which is what lets `core` depend on nothing
  but `pico_util`.
- `src/core/ntp_message.{hpp,cpp}` — the SNTP wire format and every validation
  rule, as pure functions over a 48-byte span. No lwIP, no SDK.
- `src/core/wall_clock.{hpp,cpp}` — the offset arithmetic and the publication
  protocol. No lwIP, no SDK.
- Those two headers are implementation detail: in no `FILE_SET`, never
  installed, and reached only by targets naming `src/core/` **privately** — the
  test targets and `time_sync`. On `core`'s public interface they would
  compile in-tree and vanish on install.
- `src/ntp/include/pico_time/ntp/` — the NTP client's portable half:
  `ntp_error.hpp` (the public taxonomy and `retryable()`), `ntp_transport.hpp`
  (the `INtpTransport` seam), `ntp_synchronizer.hpp` (the attempt loop and the
  slot pool). No lwIP, no SDK, so it is host-tested.
- `src/testing/include/pico_time/testing/fake_ntp_transport.hpp` — the scripted
  transport, in the install export set beside the library. A downstream project
  testing against this seam uses the installed fake on-target; host tests
  obtain it in-tree through FetchContent.
- `src/time_sync/lwip_ntp_transport.{hpp,cpp}` — the seam's lwIP side, and the
  only place `ip_addr_t` meets `NtpAddress`.
  `src/time_sync/ntp_client.cpp` — composition: wire the transport to the
  synchronizer and apply what it returns.
  `src/time_sync/time_keeper.cpp` — newlib shims.
- `src/lwipopts/` — the fallback lwIP config, reachable only through the
  explicit `pico_time_lwipopts_fallback` target.
- `tests/core/` mirrors `src/core/` (`CXX-13.2`) and holds
  `pico_time_test.cpp`, which covers the umbrella and pins the version.
  `tests/ntp/` mirrors `src/ntp/`.
- `tests/firmware/` is the cross tree's: `docs/` compiles the README's and the
  usage guide's examples from the documents (`EN-03.5`), and
  the `lwipopts-required` consumer case is AR-5's configure check.
- `examples/ntp_sync/` — flashable image for the device-only traceability rows.

Ships as `pico_time::time_sync` (firmware), and `pico_time::core`,
`pico_time::ntp` and `pico_time::testing` (portable).

## Conventions

### House policy

The rules shared by every repository that takes its gates from pico-tooling are
in [pinned family guides](../infra/agents/AGENTS.md).

### Repo specifics

- **`src/time_sync/` has no mirrored test directory, by construction.** It is
  the lwIP and newlib glue and nothing else. Every rule it applies lives in
  `src/core/`, and every decision about an attempt — how many, which datagrams
  to trust, when an abandoned lookup's slot may be reused — lives in
  `src/ntp/`, above the `INtpTransport` seam. Both are host-tested.
  What is left needs the SDK, and the traceability matrix establishes it by
  *Review* rather than by a host test. An empty `tests/time_sync/` would assert
  coverage that does not exist.

- **The slot pool is above the seam on purpose.** It exists because
  `INtpTransport::resolve()` cannot be cancelled, so a lookup fires after the
  attempt that started it has been torn down; whichever of the caller and the
  callback runs last releases the slot. That protocol is the subtlest thing here
  and a bench run cannot induce its race, so it lives where a host test can
  drive it. `FakeNtpTransport` honours the two properties it rests on: a
  resolution fires exactly once, and never while the transport's lock is held.

- **There is no battery-backed RTC.** The "wall clock" is a signed offset
  between the Unix epoch and the SDK's monotonic `time_us_64()`. Synchronizing
  stores the offset; reads add it back. Consequences that must hold: time never
  jumps backwards mid-boot unless re-synchronized, pre-sync timestamps count up
  from boot clamped non-negative, and the offset drifts with the crystal (order
  of seconds per day).
- **`std::atomic<int64_t>` is NOT lock-free on this target.** It lowers to
  `__atomic_load_8`, which the SDK implements as `spin_lock_blocking` on one
  program-wide spinlock with interrupts disabled — taken by every `time()` call
  in the binary, including the one the logger makes per line. The offset is
  therefore published through `pico_util::Published` — a four-slot ring stamped
  by a monotonic counter — and `pico_util` `static_assert`s that 32-bit atomics
  really are lock-free. A seqlock would be wrong here specifically: its reader
  spins, and an IRQ-context reader that has pre-empted its own mainline writer
  would spin forever.
- **The counter is monotonic, not a slot index.** An index that alternates can
  return to the value a reader first saw with a write to that very slot in
  between, so the reader's re-read check passes over a torn read. A counter
  never repeats, which is what makes the check conclusive.
- **Firmware is RP2350 only, and a `pico_w` configure is refused** (AR-7). The
  library declares `PLATFORM rp2350 CYW43` to the family bootstrap, because
  pico-logging's sink and level atomics are not lock-free on the RP2040 (FW-1).
- `synchronize_ntp()` is blocking, requires Wi-Fi to be up, runs up to three
  DNS+NTP exchanges each bounded by `timeout` (default 10 s), and succeeds
  immediately if already synchronized. It returns a
  `pico_util::Result<void, NtpError>` whose `operator bool` is explicit.
- **Every NTP reply is untrusted input** (`VR-1`–`VR-6`). Each attempt sends a
  fresh 64-bit nonce as its transmit timestamp and requires it back in the
  reply's originate field; that echo is the only anti-replay evidence available
  in unauthenticated SNTP. A zero transmit timestamp must be refused *before*
  the era-1 rebase, which would otherwise map it onto a valid-looking 2036.
- **No lwIP callback may log.** `PICO_LOG_*` reaches `stdout_serialize_begin`,
  which blocks for up to `PICO_STDIO_DEADLOCK_TIMEOUT_MS` (1 s) when the
  mainline holds `print_mutex` — inside an IRQ handler that has pre-empted the
  holder. Callbacks record an outcome; `synchronize_ntp()` does the logging.
- The newlib shims are the point of the library: nothing downstream should need
  to know how the clock works. Changes that require callers to opt in defeat it.
- **The portable core is host-tested; the glue is not.** `ntp_message` and
  `wall_clock` carry the whole validation surface and the publication protocol,
  so they are testable and fuzzable on a host. Claims about the lwIP client,
  the SDK counter or the newlib link still need a device run
  (`examples/ntp_sync/`), and the specification marks which rows those are.
- **`-Werror` is enforced per source, not per target.** The SDK compiles its own
  sources into whichever target links its INTERFACE libraries, so a
  target-scoped `-Werror` would land on SDK and lwIP code this repo cannot fix.
  The strict target takes `WERROR OFF`, and
  `house_cxx_werror_sources(...)` puts the flag back, per source,
  on the translation units this repo owns. Do not "simplify" it to a
  target-wide flag, and do not drop it in favour of grepping the build log —
  that grep is a backstop for a source added but never registered, not the gate.
- **Toolchain-derived `FREESTANDING`, `LANGUAGES CXX`** on the strict target are both
  load-bearing. Freestanding mode drops the hardening that assumes a hosted libc
  and a dynamic loader. `LANGUAGES CXX` keeps SDK C diagnostics — none of them
  actionable here — out of the build output.
- **`misc-include-cleaner` wants newlib internals for `timeval` and
  `suseconds_t`.** It names `sys/_timeval.h`; the POSIX-mandated spelling is
  `<sys/time.h>`, which is what the code includes. Those three sites are
  suppressed per site. The SDK cases are different and are included rather than
  suppressed: `hardware/timer.h` and `pico/types.h` are public SDK headers, so
  they are included directly alongside `pico/time.h`.
- **Lint needs the firmware tree.** The lwIP client and the newlib shims
  are firmware-only, so a host-only compile database covers the portable core
  alone and lint reads as green over the two files most worth checking.
  The compile-db gate fails on that rather than letting it pass.
- **`PICO_LWIPOPTS_TARGET` has no default, deliberately.** The SDK
  compiles lwIP into whichever target links its INTERFACE libraries — this one
  — so a fallback applied silently would put a second, differently-configured
  lwIP inside `libpico_time_time_sync.a`. Two copies of `mem.o`/`memp.o`/
  `udp.o` whose struct layouts and pool tables disagree is silent memory
  corruption resolved by archive order (`CXX-10.3`). The bundled header stays,
  but only through the explicit `pico_time_lwipopts_fallback` target.
- **include-cleaner cannot attribute a macro to its header.** The lwIP option
  `static_assert`s therefore need one `misc-include-cleaner` suppression, and
  `<lwip/opt.h>` needs a trailing `// IWYU pragma: keep` — the pragma must be
  on the include line, not above it. Testing the options against zero rather
  than one also matters: `NO_SYS == 1` preprocesses to `1 == 1`, which
  `misc-redundant-expression` reports.
- **A justification comment must sit on the line directly above a `NOLINT`,** and
  every comment line must stay inside the 80-column limit. An over-long prose
  line reflows into the `NOLINTNEXTLINE` below it, which silently separates the
  justification from the suppression and violates the suppression convention checked by review.

- **The error taxonomy is public and reaches the caller.** `NtpError` names
  every failure the client can distinguish and `retryable()` says which of them
  clear on their own, so a caller can implement the retry AR-6 prescribes.
  `NtpReject` stays private: it is the wire verdict, and `from_reject()` maps it
  onto the public vocabulary. A rejection does not end the call — `pool.ntp.org`
  resolves to a different server each lookup — so the reported error is the last
  attempt's.
