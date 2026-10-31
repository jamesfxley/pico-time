# pico-time Technical Specification

Normative. This document defines what the library guarantees; the code and
tests cite these IDs, and a guarantee that is not here is not a guarantee.

Engineering rules are cited by ID (`EN-`/`CXX-`) and resolve at
`infra/house/standard/`.

## 1. Scope and targets

A wall clock for the Raspberry Pi Pico 2 W: an SNTP client over the
lwIP raw API, and the newlib syscall shims that put its result behind
`std::chrono::system_clock`, `time()` and `gettimeofday()`.

| | |
|---|---|
| In scope | SNTP over IPv4/UDP; DNS resolution of one pool hostname; validation of server replies; a monotonic-offset wall clock readable from interrupt context |
| Out of scope | NTP server mode; symmetric or broadcast association modes; NTS or any authenticated NTP; clock disciplining (the offset is stepped, never slewed); leap-second scheduling; timezones and local time |

The board has no battery-backed RTC. Every guarantee below is about an offset
from the SDK's monotonic counter, not about a clock that survives power loss.

## 2. Functional requirements (FR) — clock behaviour

**FR-1.** The wall clock is a signed microsecond offset added to the SDK's
monotonic counter. Synchronizing stores the offset; a read adds it back.

**FR-2.** Before the first synchronization, clock reads report time elapsed
since boot rather than an epoch time.

**FR-3.** A clock read never yields a negative time. The result is clamped at
the Unix epoch.

**FR-4.** Synchronization state latches. `is_time_synchronized()` becomes true
on the first successful set and never becomes false again.

**FR-5.** A wall-clock time is accepted only when its seconds fall inside
`[MIN_EPOCH_SECONDS, MAX_EPOCH_SECONDS)` and its microseconds do not exceed
999999. A time outside those bounds is refused, and refusing leaves both the
offset and the synchronization state unchanged.

**FR-6.** The offset is published so that a reader in any execution context,
including an interrupt handler, obtains a value that was stored as a whole and
never a mixture of two. A read must not block, spin, or disable interrupts. A
read that cannot be validated is reported rather than returned: `load()`
returns `PublishedReadError::EXHAUSTED`, and `_gettimeofday` then fails with
`errno` set to `EAGAIN`, which `time()` reports as `(time_t)-1`.

That property holds while fewer than `SLOT_COUNT - 1` stores land during a
single read: the writer fills the slot *after* the one it last published, so it
takes that many stores to come round the ring to the slot a reader is in. A
read retries a bounded number of times and reports exhaustion rather than
returning what it has;
the bound is `pico_util::Published::MAX_READ_ATTEMPTS`, independent of
`SLOT_COUNT`, and exhausting it takes `SLOT_COUNT - 1` stores inside each of
those attempts.

The condition is therefore the WRITER CONTRACT in `wall_clock.hpp` — one
writer, mainline context only, at most one store per second — and it is a
statement about the writer, not a caveat on the reader. It holds by
construction only for `synchronize_ntp()`, which stores once, on success, and
then returns before any work once the clock is synchronized (NR-4), so stores
at most once per boot. `apply_wall_clock_time()` and the `settimeofday()` shim over it
are public and reach the same store with no rate limit, from whatever context
their caller runs in, so for those two the contract is an obligation on the
caller. Nothing enforces it, and no test covers its breach.

**FR-7.** Between two synchronizations the clock is monotonic: it advances with
the SDK counter and does not step. A synchronization may step it in either
direction.

**FR-8.** The width of FR-5's window is normative, not incidental. It is
`EPOCH_WINDOW_SECONDS`, ten Julian years above the floor, and it is narrower
than an NTP era. Two things rest on that. It bounds how far a forged or
malfunctioning server can move the clock, so a window wide enough to admit
anything the wire can express is no check at all. And it is what makes the era
choice of NR-5 decidable without a clock the client does not yet have.

A build whose floor is 2025-01-01 therefore refuses every time from
2035-01-01T12:00:00Z onwards, its own included: from that instant it cannot
synchronize until it is rebuilt on a later floor. Moving the window forward is
a release's job.

## 3. NTP protocol conformance (NR)

**NR-1.** A request is a 48-byte SNTP message with leap indicator 0, version 3
and mode 3 (client), per RFC 4330 §4.

**NR-2.** Each attempt draws a fresh 64-bit nonce and sends it as the request's
transmit timestamp.

**NR-3.** `synchronize_ntp()` makes at most three attempts. Each attempt is
bounded by the caller's timeout, and consecutive attempts are separated by a
one-second delay.

**NR-4.** `synchronize_ntp()` succeeds immediately, without network traffic,
when the clock is already synchronized.

**NR-5.** A transmit timestamp's seconds field carries no era number, so one
value denotes one instant per NTP era. The era is chosen as whichever reading
falls inside FR-5's window; a value that no era places inside it is refused.
FR-8 makes the window narrower than an era, so at most one reading can qualify
and no tie can arise.

A build whose window predates the 2036-02-07 rollover accepts era 0 only. What
carries the client across the rollover is moving the floor past it, at which
point the same expression starts selecting era 1 — the client never guesses.

**NR-6.** The 32-bit fraction field is converted to microseconds by integer
arithmetic. The implementation contains no floating-point operation, so no
soft-float runtime is linked on its account.

## 4. Validation and security (VR)

The trust boundary (`CXX-12.1`) is the 48 bytes of a received UDP datagram.
`parse_ntp_response()` is the sole crossing point; no code downstream of it
re-validates, and none may assume anything it does not establish.

**VR-1.** A reply whose mode is not 4 (server) is refused.

**VR-2.** A reply whose leap indicator is 3 (alarm — the server's own clock is
unsynchronized) is refused.

**VR-3.** A reply whose stratum is outside 1–15 is refused. Stratum 0 is
Kiss-o'-Death, 16 means unsynchronized (RFC 5905 §7.3), and 17–255 are reserved.

**VR-4.** A reply whose originate timestamp does not equal the nonce sent for
that attempt is refused. This is the anti-replay property: it is what
distinguishes a live reply from a captured one replayed later.

It is evaluated **first**, ahead of VR-1 to VR-3. A datagram that has not shown
it belongs to this attempt is therefore refused for that reason and no other,
which is what VR-7 rests on.

**VR-5.** A reply whose transmit timestamp seconds field is zero is refused.
Zero denotes an unset clock and must be refused before the era rebase of NR-5,
which would otherwise map it onto a valid-looking instant.

**VR-6.** A reply whose timestamp falls outside
`[MIN_EPOCH_SECONDS, MAX_EPOCH_SECONDS)` under every era is refused (NR-5).
Both edges of that window are reachable from the wire, and both are tested: a
ceiling no message can reach is a check that cannot fail.

**VR-7.** A datagram that fails the source address, source port or length check,
or that fails VR-4, does not end the attempt in progress. The client continues
waiting for a valid reply until its timeout.

VR-4 running first is what makes this cover every unauthenticated datagram
rather than only the ones that are otherwise well-formed: a forged or replayed
packet cannot deny a synchronization that would otherwise have succeeded,
whatever else is wrong with it. A reply that passes VR-4 and then fails VR-1,
VR-2, VR-3, VR-5 or VR-6 is a real server reporting an unusable state, and does
end the attempt.

**VR-8.** No lwIP callback logs, blocks, takes a lock, or allocates
(`CXX-08.3`). A callback records an outcome in plain stores and returns;
sending, reporting and tearing down all happen in the caller's context.

Plain rather than atomic, and the argument is the execution model rather than
the fields. lwIP runs in a single-core background context under the `NO_SYS`
configuration the adapter asserts, so a callback runs to completion with
respect to the mainline and there is no interleaving for an ordering to
protect; the mainline's spin on the published flags cannot be hoisted either,
because every call in that loop body leaves the translation unit through
`INtpTransport`. Both halves are load-bearing, which is what makes the `NO_SYS`
assertion a correctness guard rather than a portability note: a preemptive
arrangement — a second core servicing lwIP, or a `NO_SYS=0` build — breaks this
silently, and under one the flags must be published through
`pico_util::Published` as the wall clock's are.

One exception, and it is the only one: `pbuf_free()` on the datagram lwIP hands
the receive callback. The lwIP API requires the callback to release it, and it
returns to lwIP's own pool, which is interrupt-safe by construction — unlike
the C heap, where a free from a callback that pre-empted a mainline allocation
is a lock it may already hold. Nothing in this library reaches the C heap at
all: an attempt's state is a slot of a fixed pool sized to the attempt count,
and an attempt whose DNS lookup outlives it releases its slot with one store.

## 5. Architecture requirements (AR) — the public interface

**AR-1.** Every public entry point is `[[nodiscard]]`. Each returns a status
that is its only failure channel, since the target builds without exceptions.

**AR-2.** `synchronize_ntp()` refuses a timeout that is not positive or that
exceeds `MAX_SYNC_TIMEOUT` with `NtpError::INVALID_TIMEOUT`, without network
traffic. The check comes first, so it holds on a clock already synchronized,
ahead of NR-4.

**AR-3.** `apply_wall_clock_time()` applies FR-5 and reports refusal through its
return value.

**AR-4.** `_settimeofday()` validates its argument, and on a negative `tv_sec`,
a negative `tv_usec`, a `tv_usec` above 999999, or a time outside FR-5's window,
returns −1 with `errno` set to `EINVAL` and leaves the clock untouched.

**AR-5.** A firmware build requires `PICO_LWIPOPTS_TARGET`. Configuration
fails when it is unset, because the Pico SDK compiles lwIP into this library and
a default would produce a second lwIP built against a configuration the
application does not use (`CXX-10.3`).

The variable is the family's rather than this library's, and is declared by
`pico_family_require_lwipopts_target`. An image links at most one `lwipopts.h`,
so an application linking two networking libraries names it once; a
library-specific spelling would make it set two variables for one thing.

**AR-6.** `synchronize_ntp()` fails with `NtpError::NO_ATTEMPT_SLOT`, without
network traffic and without spending an attempt, when every attempt slot is
still held by a DNS lookup that has not landed. The pool holds
`MAX_REQUEST_ATTEMPTS` slots, and a slot outlives its attempt only while a
lookup it cannot cancel is in flight (VR-8). The condition is transient, so
`retryable()` reports it as such: a caller that cannot distinguish it from a
network failure gives up on a state that was about to clear.

lwIP calls every lookup back, with a null address on failure, and that callback
releases the slot, so a caller that retries after a few seconds succeeds.

**AR-7.** A board without an RP2350 and the CYW43 radio is refused at
configure. Synchronisation runs over the radio, and pico-logging needs the
RP2350's exclusive access, so the library declares `PLATFORM rp2350 CYW43` to
the family bootstrap, which refuses a board without what a library declares. A consumer adding it on such a board fails
at configure, naming what the board lacks, rather than in the build or on the
device. A Pico W (RP2040) is refused at configure and the library cannot build for it (FW-1).

## 6. Gates

Normative, CI-enforced. These are decisions, not measurements (`EN-03.2`).

Static RAM uses pico-tooling:AR-11: every `ALLOC` section except a
`LOAD, READONLY` section with equal VMA and LMA, `.heap` and `.stack*`.
Owned flash uses source-attributed code/read-only symbols, a separate measure.

| Gate | Bound | Enforced by |
|---|---|---|
| C-heap allocation in this library's objects | None, in any context (VR-8) | `arm-none-eabi-nm -u` over this repository's own objects references no allocator symbol — `malloc`, `calloc`, `realloc`, `free`, or `operator new`/`delete` |
| Per-attempt heap | None. Attempt state is a slot of a static pool of `MAX_REQUEST_ATTEMPTS` | The same `nm` gate |
| Soft-float in this library's objects | None (NR-6) | `arm-none-eabi-nm -u` references no `__aeabi_[df]` |
| Static RAM of the example image | <= 64 KiB | `pico.toml` footprint `ram_kib` |
| Owned code and read-only symbols attributed to this library | <= 12 KiB | `pico.toml` footprint `owned_prefix` and `owned_kib` |
| Flash (every `LOAD` section) of the example image | <= 384 KiB | `pico.toml` footprint `flash_kib` |
| Clang-tidy findings | Zero. The gate is blocking over the whole check set | `python3 dev.py lint`, over a database built from **both** trees, with a coverage check that every tracked source is in it |
| Suppressions | `docs/deviation-budget.txt`, ratcheting down only | the deviations gate |
| Requirements with no citation | None | review (EN-02) |

The two flash bounds answer different questions and only the first is this
library's to promise. The owned bound is what a consumer adds by taking
`pico-time`: it does not move when the SDK or the CYW43 firmware blob does, and
a regression in this repository's own code is the only thing that can move it.
The whole-image bound is a loose catch for a step change, sized to the example
as it is actually built rather than to an aspiration — the image is mostly SDK
and firmware blob, and holding it to this library's own cost would fail a gate
on an unchanged tree.

The `nm` gates read this repository's own objects only. The Pico SDK compiles
its own sources into whichever target links its INTERFACE libraries, so the
build directory also holds objects for `pico_double`, `new_delete` and the rest
of the C++ runtime — `new_delete.cpp.o` references `malloc` by definition, and
none of them is ours to change.

## 7. Traceability matrix

| ID | Established by | Criterion |
|---|---|---|
| FR-1 | `wall_clock_test.cpp` `ComputeEpochOffset.ProducesTheOffsetBetweenEpochAndUptime`, `ApplyOffset.AddsTheOffsetToTheMonotonicReading` | Offset equals epoch microseconds minus the monotonic reading, and a read adds it back to the monotonic reading |
| FR-2 | Device run | Pre-synchronization `time()` reads seconds since boot, not an epoch time. The run on this release is pending (FW-3) |
| FR-3 | `wall_clock_test.cpp` `ApplyOffset.ClampsANegativeResultToTheEpoch` | A negative sum yields 0 |
| FR-4 | `wall_clock_test.cpp` `WallClock.StartsUnsynchronized`, `WallClock.PublishesAnOffsetAndLatchesSynchronized` | `synchronized()` is false before any store and true after one. That it never returns to false is structural — nothing clears the flag — and is review-only |
| FR-5 | `wall_clock_test.cpp` `ComputeEpochOffset.EnforcesTheSecondsWindowAtItsEdges`, `.RejectsOutOfRangeMicroseconds`, `.RejectsTheSecondsForMicrosecondsMixUp` | Both window edges and 999999/1000000 classify correctly; the microseconds-for-seconds mix-up is refused rather than overflowing. That a refusal leaves the offset and the synchronization state unchanged is review-only: `apply_wall_clock_time` returns before it stores |
| FR-6 | `wall_clock_test.cpp` `WallClock.ReadsAreNeverTornUnderAConcurrentWriter`, `.CarriesTheFullSignedRangeAcrossTheSplit`; review of `pico_util::Published`'s bounded retry | Under a concurrent writer no reader observes a value that was never stored; every value round-trips across the 32-bit split. That a read does not block, spin or disable interrupts is structural — the retry is bounded by `MAX_READ_ATTEMPTS` and the words are 32-bit atomics — and is review-only. A read that cannot be validated is reported, and `_gettimeofday` fails with `EAGAIN` on it: that path is **review-only**, since `time_keeper.cpp` builds for the target alone and exhaustion needs a writer breaking its contract mid-read, which no host case produces deterministically. |
| FR-7 | Device run | Successive reads increase across an idle period. The run on this release is pending (FW-3) |
| FR-8 | `ntp_message_test.cpp` `NtpMessage.ThePlausibilityWindowIsNarrowerThanAnNtpEra`, `.RefusesEveryEra1FieldWhileTheWindowPredatesTheRollover` | The window is narrower than an era, and a window below the rollover admits no era-1 field |
| NR-1 | `ntp_message_test.cpp` `NtpMessage.BuildsAClientRequestCarryingTheNonce` | First byte is 0x1B |
| NR-2 | `ntp_message_test.cpp` `NtpMessage.BuildsAClientRequestCarryingTheNonce`; `ntp_synchronizer_test.cpp` `NtpSynchronizer.GivesEachAttemptItsOwnNonce` | The nonce occupies bytes 40–47 big-endian, and a fresh one is drawn for each attempt. That the request sent carries the attempt's own nonce is review-only, of `ntp_synchronizer.cpp` |
| NR-3 | `ntp_synchronizer_test.cpp` `TriesEveryAttemptBeforeGivingUp`, `WaitsBetweenAttemptsButNotAfterTheLast` | An unreachable server is retried `MAX_REQUEST_ATTEMPTS` times with `MAX_REQUEST_ATTEMPTS - 1` waits between them and none after the last. Transport-independent, which is what the seam is for; no device run is kept for it |
| NR-4 | Device run | A second call returns without a DNS query. The run on this release is pending (FW-3). Direct duration and packet capture are absent (FW-2) |
| NR-5 | `ntp_message_test.cpp` `NtpMessage.RefusesEveryEra1FieldWhileTheWindowPredatesTheRollover`, `.AcceptsATimestampExactlyAtThePlausibilityFloor`, `.AcceptsTheSecondBelowThePlausibilityCeiling` | Both sides of the era boundary are refused under this window; both edges of the window are accepted through era 0 |
| NR-6 | `ntp_message_test.cpp` `NtpMessage.ConvertsTheFractionExactly`; `arm-none-eabi-nm -u` over this library's objects | The conversion table holds at 0, ¼, ½, ¾ and full scale; no `__aeabi_[df]` symbol is referenced |
| VR-1 | `ntp_message_test.cpp` `NtpMessage.RejectsAnythingButAServerReply` | Modes 0, 3 and 5 are refused with `MODE` |
| VR-2 | `ntp_message_test.cpp` `NtpMessage.RejectsTheLeapAlarmCondition`, `.AcceptsThePendingLeapSecondIndicators` | LI 3 refused; LI 0–2 accepted |
| VR-3 | `ntp_message_test.cpp` `NtpMessage.RejectsUnusableStrata`, `.AcceptsPrimaryAndSecondaryStrata` | 0, 16, 17 and 255 refused; 1 and 15 accepted |
| VR-4 | `ntp_message_test.cpp` `NtpMessage.RejectsAReplyToADifferentRequest`, `.RejectsAReplayedCapture`, `.RequestAndResponseAgreeOnWhereTheNonceLives`, `.AnUnauthenticatedDatagramIsOnlyEverAnOriginateMismatch` | A mismatched nonce is refused; a reply valid under one nonce is refused under another; a built request round-trips; a mismatched nonce outranks every other defect in the message |
| VR-5 | `ntp_message_test.cpp` `NtpMessage.RejectsAnUnsetTransmitTimestamp`, `.RejectsAnUnsetTimestampEvenWithANonZeroFraction` | A zero seconds field is refused whatever the fraction |
| VR-6 | `ntp_message_test.cpp` `NtpMessage.RejectsATimestampBelowThePlausibilityFloor`, `.AcceptsATimestampExactlyAtThePlausibilityFloor`, `.AcceptsTheSecondBelowThePlausibilityCeiling`, `.RejectsATimestampAtAndAboveThePlausibilityCeiling` | Both edges classify correctly, and both are reachable from the wire |
| VR-7 | `ntp_message_test.cpp` `NtpMessage.AnUnauthenticatedDatagramIsOnlyEverAnOriginateMismatch`; `ntp_synchronizer_test.cpp` `ADatagramFromElsewhereDoesNotEndTheAttempt`, `ADatagramTooShortForAMessageDoesNotEndTheAttempt`, `AReplyToAnotherRequestIsDroppedNotCounted` | No unauthenticated datagram reaches a verdict other than `ORIGINATE_MISMATCH`, and both that path and the address and length checks return without publishing an outcome. The port check is in the lwIP receive callback and is review-only |
| VR-8 | Review of the lwIP callbacks — `lwip_ntp_transport.cpp` `on_dns_resolved` and the `udp_recv` lambda, `ntp_synchronizer.cpp` `Slot::on_resolved` and `Slot::on_datagram`; the C-heap `nm` gate | No callback contains a `PICO_LOG_*`, a lock acquisition, a send, or an allocation other than the required `pbuf_free`; no object of this library references the C heap. `ntp_client.cpp` is composition, not a callback, and logs deliberately |
| AR-1 | ctest `pico_time_refuses_discarded_synchronized`; the compiler, through pico-util's `[[nodiscard]]` `Result` | Discarding `is_time_synchronized()` fails to compile with `nodiscard`, while the same call used compiles; every other entry point returns a `Result`, whose discard pico-util refuses, and warnings are errors |
| AR-2 | `ntp_synchronizer_test.cpp` `RefusesANonPositiveTimeoutWithoutTouchingTheNetwork`; review of `ntp_client.cpp` `synchronize_ntp` for the ceiling and the order | A zero and a negative timeout are each refused with `INVALID_TIMEOUT` before any network call. The `MAX_SYNC_TIMEOUT` ceiling is review-only: it bounds how long a caller may surrender its core, which is a policy of the public entry point rather than of the transport-agnostic synchronizer |
| AR-3 | Review of `time_keeper.cpp` `apply_wall_clock_time` | It returns `MICROSECONDS_INVALID` for an out-of-range microseconds field and `EPOCH_OUT_OF_RANGE` for an out-of-window epoch, so a caller who passed microseconds-since-epoch into the seconds argument can find that mistake. Review-only: `pico_time_time_sync` is not in the host build — it compiles lwIP and SDK sources — so the only other reference is the firmware consumer smoke build, `tests/consumer/consumer_smoke.cxx`, where linking is the assertion |
| AR-4 | Review; `time_keeper.cpp` `_settimeofday` | Each refused case returns −1, sets `EINVAL` and returns before the clock is stored |
| AR-5 | `lwipopts-required` consumer case, a firmware configure without the variable | Configuration fails with the diagnostic naming the variable |
| AR-6 | `ntp_synchronizer_test.cpp` — `ALookupThatNeverLandsHoldsItsSlot`, `SlotExhaustionIsReportedAndIsRetryable`, `AnAbandonedLookupReleasesItsSlotWhenItLands`, `AResolutionLandingUnderTheLockIsSerialized` | Three attempts abandoned in resolution hold all three slots; the next call fails with `NO_ATTEMPT_SLOT`, opens no endpoint, and reports the failure as retryable; once each abandoned lookup lands its slot is released and a later call synchronizes normally, and a lookup landing under the lock is serialized behind it. That a slot is released exactly once is review-only, of the `resolve_pending` and `abandoned` handshake under the lock. Established here rather than by a device run: a lookup landing after the attempt that started it was torn down is a race the bench cannot induce on demand |
| AR-7 | `pico-w-refused` and `pico2-refused` consumer cases, a consumer adding the library | On `pico_w` the configure fails saying `PICO_TIME needs PICO_PLATFORM rp2350`; on `pico2`, saying `PICO_TIME needs a board with the CYW43 radio` |

Every row whose criterion is a **device run** requires hardware and is
established by the on-device validation in `examples/ntp_sync/`, not by the host
suite (`EN-06.6`). For this release those runs are pending (FW-3). Rows marked **Review** state a structural property of a
firmware-only translation unit that no host test can reach; they are checked by
the
pinned source review guide (`infra/house/guides/review/source.md`).

Requirement citations are checked by review (EN-02.1).
