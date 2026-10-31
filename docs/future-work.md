# Future work

Work deliberately not done, one entry per deferral, in the form `EN-03.7`
defines.

## FW-1 — Pico W (RP2040) validation

### What is absent

The library is refused on the Pico W (RP2040) at configure (AR-7), and no
functional run exists on that board.

### Why it is not built

pico-time depends on pico-logging, whose lock-free assertions do not hold on
the RP2040's ARMv6-M core (pico-logging:FW-1), so the library cannot build for
that board until that entry is built. Validation then needs a Pico W on the
bench.

### What would have to be true to build it

pico-logging:FW-1 is built. Then `PLATFORM rp2350` is dropped from this
library's bootstrap, the functional NTP suite runs on a Pico W, and AR-7's
RP2350 clause is re-specified.

### Version cost

Not breaking under `EN-06.1`: it admits one more board and removes no accepted
configuration.

### Affected repositories

pico-time; pico-logging:FW-1.

## FW-2 — NR-4 direct duration and packet capture

### What is absent

NR-4 requires that a second `synchronize_ntp()` succeeds immediately and
without network traffic. Neither the call's duration nor the absence of packets
has been measured directly: log timestamps around the call include logging, and
no packet capture exists.

### Why it is not built

The no-network property rests on `src/time_sync/ntp_client.cpp`, which returns
before transport initialization when the clock is synchronized. A direct
measurement needs a device run with a timing probe and a capture of the
board's traffic.

### What would have to be true to build it

A device run that times the second synchronization directly, and a DNS and
packet capture over the same interval that shows no traffic.

### Version cost

Not breaking under `EN-06.1`: it adds evidence and changes no interface.

### Affected repositories

pico-time.

## FW-3 — FR-2, FR-7 and NR-4 device run on this release

### What is absent

No run record of `examples/ntp_sync/` on a board is committed for FR-2 (time
counts from boot before the first synchronization), FR-7 (successive reads
increase across an idle period) or NR-4's functional criterion (a second call
succeeds immediately). The specification names a device run as the
establishing evidence for all three. FW-2 covers NR-4's direct duration and
packet capture.

### Why it is not built

The run needs a board, a Wi-Fi network and a flashing session, and its
capture must be armed before the flash. None has been recorded against this
release.

### What would have to be true to build it

A device run of `pico_time_ntp_sync` that records the pre-synchronization and
post-synchronization epochs, the epoch after the idle wait and the outcome of
the second synchronization, committed as a run record.

### Version cost

Not breaking under `EN-06.1`: it adds a record and changes no interface.

### Affected repositories

pico-time.
