# ntp_sync device validation

This image exercises the SDK clock, radio and newlib hooks. The
[specification](../../docs/technical-specification.md) owns the guarantees.
[Pending device evidence](#pending-device-evidence) describes the missing run
records.

## Building and flashing

Build in the devcontainer with the SDK and toolchain. Device actions require
separate authorisation under the [family device guide](../../infra/guides/device.md).
The commands below write `build-device/`. Credentials must be supplied at
CMake configuration time:

```sh
set -a; . ./.env; set +a
cmake -S . -B build-device -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPICO_SDK_PATH="$PICO_SDK_PATH" \
    -DPICO_BOARD=pico2_w \
    -DPICO_LWIPOPTS_TARGET=pico_time_lwipopts_fallback \
    -DPICO_TIME_WIFI_SSID="$WIFI_SSID" -DPICO_TIME_WIFI_PASSWORD="$WIFI_PASSWORD"
cmake --build build-device
```

Verify the SSID in the image before flashing. A stale cache can prevent
association with the same symptom as an incorrect password:

```sh
strings build-device/examples/ntp_sync/pico_time_ntp_sync.elf | grep -cF -- "${WIFI_SSID:?set WIFI_SSID}"
```

Capture serial output before flashing. The board has no serial port while in
BOOTSEL mode, so capture needs to wait for the port to enumerate. All output
occurs at boot; attaching afterwards can miss it.

## Expected log

| Row | What the log must show |
|---|---|
| Pre-sync clock | `pre-sync: epoch=<small>` reports time since boot |
| Monotonic reads | `post-sync` and `after 3s` differ by exactly 3 with no backward step |
| Repeat call | The second `synchronize_ntp()` succeeds immediately, with no DNS or NTP log lines between |

The specification assigns the pre-sync clock and monotonic-read rows to
`tests/device/`.

Host tests `TriesEveryAttemptBeforeGivingUp` and
`WaitsBetweenAttemptsButNotAfterTheLast` establish retry structure. The
[specification](../../docs/technical-specification.md#7-traceability-matrix)
names their coverage. The table describes what each row's output should show.

A local 1 ms timeout probe cannot guarantee that every attempt times out:
cached DNS and a fast responder can complete sooner. Use the scripted host
tests for deterministic retry checks.

Slot exhaustion needs lookups that remain outstanding across attempts. A local
probe can select an unroutable DNS server. This example contains neither that
edit nor a capture of it.

## USB servicing

`main()` waits indefinitely instead of returning, so the SDK continues servicing
USB. The serial tail remains readable and the 1200-baud BOOTSEL touch remains
available after early failure.

## Pending device evidence

The missing example run and the separate duration and packet-capture evidence
are owned by [future work](../../docs/future-work.md).
Capture must be armed before flashing.
