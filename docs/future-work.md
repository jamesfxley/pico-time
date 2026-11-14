# Future work

Deferred work follows [EN-03.7](../infra/house/standard/engineering/03-documentation.md).

## FW-1 — Pico W (RP2040) validation

### What is absent

Configuration fails on the Pico W (RP2040), and no functional run exists on
that board (AR-7).

### Why it is not built

pico-time depends on pico-logging, whose lock-free assertions do not hold on
the RP2040's ARMv6-M core (pico-logging:FW-1), so the library cannot build for
that board until that entry is built. Validation then needs a Pico W on the
bench.

### What would have to be true to build it

pico-logging completes RP2040 support. The library bootstrap can then omit
`PLATFORM rp2350`, the NTP suite can run on a Pico W, and the RP2350 requirement
can be revised (pico-logging:FW-1, AR-7).

### Version cost

Not breaking under `EN-06.1`: it admits one more board and removes no accepted
configuration.

### Affected repositories

pico-time; pico-logging:FW-1.

## FW-2 — NR-4 direct duration and packet capture

### What is absent

NR-4 requires a second `synchronize_ntp()` call to succeed immediately without
network traffic. Neither the call's duration nor the absence of packets
has been measured directly: log timestamps around the call include logging, and
no packet capture exists.

### Why it is not built

The no-network behaviour follows from `src/time_sync/ntp_client.cpp`, which returns
before transport initialisation when the clock is synchronised. A direct
measurement needs a device run with a timing probe and a capture of the
board's traffic.

### What would have to be true to build it

A device run times the second call directly and captures DNS and NTP traffic
over the same interval, showing no packets.

### Version cost

Not breaking under `EN-06.1`: it adds evidence and changes no interface.

### Affected repositories

pico-time.

## FW-3 — NR-4 device run on this release

### What is absent

No run record of `examples/ntp_sync/` on a board is committed for NR-4's
functional criterion (a second call succeeds immediately). The specification
names a device run as its establishing evidence. FR-2 and FR-7 are established
by the device tests in `tests/device/`. FW-2 covers NR-4's direct duration and
packet capture.

### Why it is not built

The run needs a board, a Wi-Fi network and a flashing session, and its
capture must be armed before the flash. None has been recorded against this
release.

### What would have to be true to build it

A committed device run record shows the outcome of the second call to
`synchronize_ntp()` in `pico_time_ntp_sync`.

### Version cost

Not breaking under `EN-06.1`: it adds a record and changes no interface.

### Affected repositories

pico-time.
