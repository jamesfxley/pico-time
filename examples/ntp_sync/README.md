# ntp_sync — device validation

A flashable image that establishes the traceability rows in
[`docs/technical-specification.md`](../../docs/technical-specification.md) whose
criterion is a device run. Those rows are properties of the SDK's monotonic
counter, the radio and the newlib link — not of the portable core — so no host
test can reach them.

The specification is normative. This file holds the procedure and what each
row's output should show; the run record for this release is pending (FW-2,
FW-3). Nothing here is a commitment.

## Building and flashing

Build in the dev container, which carries the SDK and toolchain. Device actions
require separate authorization; see the [family device guide](../../infra/guides/device.md).
The build below writes `build-device/` and names its image explicitly.
Credentials are consumed at **cmake-configure** time, so they must be passed to
the configure and not to the build:

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

Verify the credentials actually reached the image before flashing — a stale
cache produces firmware that cannot join, and the symptom is indistinguishable
from a bad password:

```sh
strings build-device/examples/ntp_sync/pico_time_ntp_sync.elf | grep -cF -- "${WIFI_SSID:?set WIFI_SSID}"
```

**Start the serial capture before the flash.** The board is a mass-storage
device until the `.uf2` lands, so there is no port to attach to beforehand;
the capture has to poll for the port and grab it as it enumerates. Firmware
logs at boot and is silent afterwards, so attaching later misses exactly the
lines the run exists to collect.

## What it establishes

| Row | What the log must show |
|---|---|
| `FR-2` | `pre-sync: epoch=<small>` — time since boot, not a 2020s date |
| `FR-7` | `post-sync` and `after 3s` differ by exactly 3, with no backward step |
| `NR-4` | The second `synchronize_ntp()` succeeds in the same millisecond, with no NTP activity between |

`NR-3` is established on the host, by `TriesEveryAttemptBeforeGivingUp` and
`WaitsBetweenAttemptsButNotAfterTheLast` — the retry structure is
transport-independent, which is what the seam is for. **No capture of it is
kept here**, and the row above says what one would show rather than what one
did. Reproduce it by temporarily calling
`pico_time::synchronize_ntp(std::chrono::milliseconds(1))` before the normal
call: a 1 ms timeout cannot complete a DNS+NTP exchange, so every attempt times
out and the retry structure becomes observable. That probe is a local edit, not
part of this example.

Two structural properties of the attempt-slot pool need a second local edit,
because a working DNS server makes them unreachable: point lwIP at an
unroutable DNS server as well, so that no lookup ever completes and every
attempt leaves its slot to a callback that has not landed. That probe is a
local edit too, and no capture of it is kept here.

## Parking rather than returning

`main()` never returns. Returning ends the program and with it the SDK's
servicing of USB: the tail of the log is lost, and the 1200-baud BOOTSEL touch
stops working, so a board that failed early can only be recovered by physically
replugging it. Parking keeps the port alive and the board reflashable, which is
what lets a run that ends in `park("Wi-Fi association failed")` be reflashed
from the host rather than by hand at the board.

## Pending device evidence

No run record of `pico_time_ntp_sync` on this release is committed. Two items
are pending:

- FR-2, FR-7 and NR-4's functional criterion need a device run that records
  the pre-sync and post-sync epochs, the epoch after the wait and the second
  call's outcome (FW-3).
- NR-4's direct timing and a packet capture over the same interval are
  separate (FW-2).

Capture must be armed before the flash.
