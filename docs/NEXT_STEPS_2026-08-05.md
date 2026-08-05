# Current next steps — 2026-08-05

**Status:** **AUTHORITATIVE** as of 2026-08-05 — all four sequencing decisions (D1-D4) are
accepted. This is the live execution sequence.

**Baseline:** `main` at `c3c6e92` (`docs: mark NEXT_STEPS as historical, and fix what points at it`)

**Replaces:** the forward-looking portion of [NEXT_STEPS.md](NEXT_STEPS.md), which remains a
historical record of the ESP32 Phase B/C work and the decisions learned from it.

This document answers one question: **what should be done next, and in what order?** The durable
target order and migration rules remain in [MIGRATION.md](MIGRATION.md), known defects remain in
[FOLLOWUPS.md](FOLLOWUPS.md), and the evidence behind the immediate correctness work remains in
[CORRECTNESS_REVIEW_2026-08-04.md](CORRECTNESS_REVIEW_2026-08-04.md).

## Executive sequence

1. Freeze the observable BLE-advertisement baseline and settle the decisions that would change
   the first two shared interfaces.
2. Implement the portable F4 advertising/lifecycle controller as the first real source in
   `shared/`.
3. Promote `od_config.c` as the first **protocol-behavior** subsystem, closing F3 with tests.
4. Close the remaining ESP32 correctness findings in transport, event handoff, lifecycle truth,
   secure erase, and mDNS.
5. Run the release acceptance matrix and explicitly record any hardware debt still accepted.
6. Import nRF54L15 into `targets/nordic-zephyr/`, one subsystem at a time.
7. Import EFR32BG22, using its no-kernel and 32 KB RAM limits as hard shared-core gates.
8. Decide the deployed nRF52840 product split, then port only the population that is actually
   meant to move to Zephyr.

The wire-capture corpus, host issues, protocol-header synchronization, CI matrix, and BG22
security decision run in parallel, with deadlines stated below.

## Current state

| Area | State on 2026-08-05 | Consequence |
|---|---|---|
| ESP32 framework port | Phase B and ESP32 Phase C app work complete | Stop treating shim removal as the next milestone |
| Arduino shim ratchet | 5, the verified floor; remaining uses are nRF-only guarded arms | It reaches zero only when the nRF52840 code leaves at migration step 4 |
| ESP32 builds | Ten board fragments build | `s3-e1004` remains an eleventh source variant with no fragment |
| ESP32 hardware | S3 boot, encrypted compressed push, config, reboot, and deep-sleep wake exercised | Uncompressed push and interrupted recovery were closed unrun by explicit risk acceptance |
| Hardware defects | FastEPD pixel-path fix is not hardware-verified; `bbepWaitBusy` still blocks the loop | These remain named release risks, not invisible assumptions |
| `shared/` | Source list empty | The next shared source establishes the real build/test pattern |
| Host tests | Header canary, vector replay where Python permits it, and link-owner sanitizer tests | The first controller/config sources need real C unit suites and fakes |
| Correctness review | F1 and F2 implementation fixes are on `main`; F3-F10 remain open | Correctness closure precedes declaring the ESP32 target release-ready |
| Nordic/Zephyr | README scaffold only | Real target import is migration step 2 |
| Silabs | README scaffold only | Real target import is migration step 3 |
| nRF52840 | Still part of the source `Firmware` tree and Bluefruit-based | Its guarded shim arms cannot be converted or deleted blindly on ESP32 |

The previous Gate 2 is not retroactively declared passed. The decision to close its last two
items unrun permits work to continue; it does not turn missing evidence into positive evidence.
Every new subsystem promotion below still has its own host and hardware acceptance bar.

## Sequencing decisions made by this plan

### D1 — F4 is the first shared source; config remains the first protocol promotion — **ADOPTED 2026-08-05**

[F4_PORTABLE_BLE_LIFECYCLE_PLAN.md](F4_PORTABLE_BLE_LIFECYCLE_PLAN.md) is **adopted** as the
resolution of the previously contradictory ordering. `shared/core/od_adv_control.c` lands before
`od_config.c`.

The justification is **architectural, not risk-based**, and stating it accurately matters
because the plain reading of severity says the opposite:

- the controller is plain C, run-to-completion, statically allocated, and encodes no NimBLE or
  RTOS execution model — so it is the cleanest possible first tenant of `shared/`;
- its host tests and fake HAL establish the shared-source testing pattern **before** protocol
  state is moved, which is worth more than the order of the two fixes;
- the canonical 16-byte MSD and on-air AD layout do not change, so it carries no wire risk; and
- Bluefruit, Zephyr, and BGAPI consume the same controller when their real targets arrive.

**What this ordering is NOT justified by.** An earlier draft argued F4 goes first because it is
"a live C++ data race." It is a real defect, but the severity case is weaker than it appears and
should not be leaned on: both tasks are pinned to core 0 on the S3 baselines
(`CONFIG_BT_NIMBLE_PINNED_TO_CORE=0`, `CONFIG_ESP_MAIN_TASK_AFFINITY=0x0`), making these
preemption races rather than cross-core visibility failures, and the shipped Arduino firmware
ran a strictly worse version of the same race — a `std::vector` advertisement payload, a
`bool:1` flag, and no lock anywhere in `NimBLEAdvertising` — with no known field failure. F4 is
being done first because it is the right *first shared component*, not because it is the most
dangerous open finding.

**The consequence to keep in view:** F3 is High, and this ordering puts its closure in Milestone
2, behind a Medium. Pulling F3's fix forward was considered and **rejected by decision on
2026-08-05** — it closes with the config promotion. The exposure that creates, and the fact that
the deferral currently has no review trigger, are recorded in Milestone 2.

`od_config.c` is still first among subsystems that parse, store, or alter wire behavior. The
config-first security rationale in the historical plan therefore remains intact.

### D2 — old hardware gaps are accepted debt, not prerequisites — **CONFIRMED 2026-08-05**

The uncompressed-push and interrupted-recovery runs do not block starting F4. They remain in the
system acceptance matrix and must be reported as `PASS`, `FAIL`, or `ACCEPTED-UNRUN`; they must
not disappear merely because the old item was closed.

Confirmed as accepted debt. The two paths stay named in Milestone 4's hardware matrix and are
reported `ACCEPTED-UNRUN` until someone runs them — not omitted, and not inferred from the
compressed-push result that did pass.

### D3 — no silent security-deferral rollover — **RE-TAKEN 2026-08-05: DEFERRED**

The recommendation was to hotfix now. **The decision is to defer**, and the point of D3 was that
the deferral be taken deliberately rather than inherited. It has been.

What is being deferred, named exactly so it is not re-litigated from memory (MIGRATION.md
§ "Risks to watch"):

- the **`<31 byte` plaintext authentication bypass**, and
- **a session surviving a key change**.

Both are Silabs-specific and both are on **shipped, field-updatable** hardware: `.gbl` through
the AppLoader is the update path HA actually ships, so these are reachable by a release rather
than a bench visit. That is what made the original "expensive to fix early" justification stop
applying, and it still does not apply — deferring now is a decision to leave a reachable
authentication bypass in the field until migration step 3, which is milestones away.

That is a legitimate call to make; it is recorded here so it is visible rather than implicit.

**This deferral is incomplete as recorded.** D3 requires four fields for a kept deferral and
none is yet set:

| Field | Value |
|---|---|
| Owner | **UNSET** |
| Expiry date | **UNSET** |
| Affected released versions | **UNSET** — needs the shipped BG22 version list |
| Release expected to remove the exposure | **UNSET** — nominally migration step 3 (Milestone 6) |

Until these are filled in, the deferral has no review trigger and will roll over silently, which
is the exact failure D3 exists to prevent. Fill them in or the milestone-0 exit gate ("the BG22
security choice has an owner and deadline") is not met.

### D4 — target-specific policy decisions precede shared APIs

Two of these are now **CONFIRMED (2026-08-05)** and are no longer open questions:

- **LAN PIPE is REJECTED.** `0x0080`/`0x0081`/`0x0082` must be refused on both plaintext and
  TLS LAN origins, as the canonical protocol already requires. No protocol or host change is
  sought. *Repo state: both documents already agree — `DIVERGENCE_MATRIX.md` 9.4 requires the
  dispatcher gate, and the canonical header's LAN rule (2) forbids the opcodes. The
  **implementation** does not: `communication.cpp` dispatches all three with no origin test.
  This is now purely an implementation defect (F5, first half) with nothing left to decide.*
- **SECOND CLIENTS ARE REFUSED, never evicted.** A live owner is never displaced by a new
  connection; refusal is inert and must not perturb the incumbent. *Repo state: the
  implementation already does this (`admitOrRefuseLanClient`), and `CONNECTION_POLICY.md` R3
  already makes it normative. **The canonical protocol header is now the sole dissenter** —
  its LAN rule (3) still says "a new connection EVICTS the prior one". That text is wrong as
  of this decision and must be changed upstream in `../opendisplay-protocol`; it cannot be
  fixed here, both because the headers are vendored copies and because they are frozen. Until
  it changes, a conforming host written to the canonical text will expect eviction and be
  refused. See the parallel-work table.*

A third is now **CONFIRMED (2026-08-05)**:

- **`MAX_CONFIG_CHUNKS` becomes 21, so the full 4,096 bytes are transferable.** The 20/4,096
  mismatch is resolved in favour of the size, not the chunk count: `MAX_CONFIG_SIZE` is 4,096 on
  every target by deliberate decision, and 20 x 200 = 4,000 leaves the last 96 bytes
  unreachable. **The canonical header is updated later** — it is frozen, and
  `shared/protocol/opendisplay_protocol.h:889` is a vendored byte-for-byte copy that must never
  be hand-edited.

  Three consequences, all of which bind before the change lands:

  1. **The ceiling is 4,000 until canonical changes.** Tests, host-facing documentation, and any
     capability statement must say 4,000 today. Do not describe 4,096 as transferable on the
     strength of this decision.
  2. **`shared/protocol/` must join the sync tool's copy map FIRST.** The map still lists only
     the four original repos, so a local bump to 21 here would diverge from canonical with
     **nothing able to detect it** — `--check` does not police this repo. Bumping before the
     map is wired converts a tracked constant into silent drift, which is worse than the
     unreachable 96 bytes. This promotes the copy-map task from parallel work to a prerequisite.
  3. **Rollout is firmware-first, then host.** A device accepting 21 chunks is strictly more
     permissive, so an old host is unaffected; a host that sends 21 chunks to an un-updated
     device is NACKed at the twenty-first frame. Deployed BG22 units additionally truncate at
     2,048 until they take an update, so the host cannot infer capability from version alone.

The last one is **DEFERRED (2026-08-05)**:

- **The size-table skip model for unknown config packets is deferred**, because it is an
  incompatible wire change. Deployed firmware has no size table and cannot skip a packet type it
  does not know, so the graceful-skip behaviour only exists on devices that take an update —
  which is precisely what makes it a wire break rather than a compatible addition.

  **The cost, stated because it removes part of Milestone 2's rationale:** the historical plan
  put `od_config.c` first partly because "the NRF54 size-table parser lands with it, which is
  what re-opens *add a new config packet type* as a safe move." With the skip model deferred,
  **the config promotion no longer re-opens that move.** Adding a new config packet type stays
  unsafe against deployed firmware until this lands. The promotion's other reasons — pre-auth
  attack surface, F3 closure, the vector/fuzz workflow — are unaffected.

  Current behaviour is therefore unchanged and remains a known divergence: ESP32 does not parse
  `0x2A` and skip-to-CRC discards the rest of the blob (`DIVERGENCE_MATRIX.md` § 2.1,
  `FOLLOWUPS.md` § 3.4). Do not partially implement the skip model as a side effect of the
  config promotion.

**D4 is now fully settled** — two confirmed, one confirmed-pending-canonical, one deferred. No
item in it remains open.

## Milestone 0 — freeze evidence and close interface decisions

This milestone is deliberately short. It prevents the first shared code from baking an
unreviewed assumption into every target.

### Required work

1. Capture exact ESP32 advertising and scan-response bytes for the stock name and 16-byte MSD.
   Record both passive-scan and active-scan observations. This is the byte fixture for F4; the
   current implementation keeps flags, name, and MSD in ADV and puts the 128-bit service UUID in
   the scan response.
2. Choose and document the corpus schema (`forbids`, `expect.parsed`, and one frame per vector).
   Capture the ESP32 protocol sessions that will be replaced by `od_config.c` before replacing
   them.
3. ~~Resolve D4's LAN admission/PIPE behavior~~ — **DONE 2026-08-05.** Both are confirmed (see
   D4) and both repo documents already state them correctly, so no repo edit is needed. What
   this leaves is not a decision but two work items: the missing PIPE origin gate (F5, Milestone
   3A) and an upstream correction to the canonical header's eviction text (parallel work).
4. ~~Re-take the BG22 security-hotfix decision~~ — **DONE 2026-08-05: deferred** (D3). The
   decision itself is made; what remains for this gate is recording the owner, expiry date,
   affected released versions, and the release that removes the exposure. Without those the
   deferral has no review trigger.
5. ~~Decide the 20-versus-21 chunk count~~ — **DONE 2026-08-05: it becomes 21** (D4). What
   remains for this gate: state the interim **4,000-byte** ceiling in tests and host-facing
   documentation, and **add `shared/protocol/` to the sync tool's copy map before any bump**,
   since nothing currently detects drift between this vendored copy and canonical.
6. ~~Land F3's `totalSize` enforcement as a target-local fix~~ — **DEFERRED 2026-08-05 to
   Milestone 2.** F3 is not fixed ahead of the config promotion; it closes with it. See the
   deferral record in Milestone 2.
7. ~~Propagate D1's promotion order into `CLAUDE.md` and `MIGRATION.md`~~ — **DONE 2026-08-05**
   (commit `5cf6ded`). Both now name `od_adv_control.c` as the first `shared/` source, and
   `CLAUDE.md` points at this document as the live sequence.

### Exit gate

- the ADV/scan-response byte fixture exists;
- the corpus schema is fixed and non-empty config captures exist;
- every D4 item has one named behavior rather than two competing descriptions; and
- the BG22 security choice has an owner and deadline.

Protocol capture may continue after this gate, but the specific surface about to be replaced
must be captured before its replacement lands.

## Milestone 1 — portable BLE advertising and lifecycle control

Implement the F4 plan test-first. This is the first real source in `shared/`, but it must not
pull the rest of BLE into shared code.

### Merge sequence

1. Add failing host tests for the loop-owned advertising state machine and a scripted fake HAL.
2. Add `shared/core/od_adv_control.c/.h` and list it in `shared/sources.cmake`.
3. Make every current build consumer compile it: ESP32 and the host suite today; retain explicit
   integration instructions for the README-only Zephyr and Silabs targets.
4. Add the ESP32 callback-to-loop bridge and a generation-stamped identity snapshot.
5. Remove OpenDisplay advertising start, stop, rebuild, and restart-policy calls from NimBLE
   callbacks. Callbacks publish facts only.
6. Route ESP32 ADV/scan-response packing and NimBLE operations through the target HAL.
7. Add the bounded teardown barrier, and make stop/deinit failures propagate truthfully through
   `BleTransport::end()` or its replacement. This closes F7 alongside F4 rather than creating a
   second lifecycle rewrite.
8. Preserve the Bluefruit and Zephyr adapter requirements as compile-tested HAL fakes now; land
   their real adapters only with their real targets, where they can be verified.

### Host acceptance

- start before ready is remembered;
- stop dominates late ready/ended/disconnect events;
- several MSD updates coalesce to one complete latest snapshot;
- disconnect during refresh defers restart until allowed;
- reset invalidates applied stack state without losing application intent;
- `ALREADY`, `NOT_ACTIVE`, retry, and hard-error HAL results preserve truthful state;
- teardown reaches quiescence or reports a bounded failure; and
- a pthread-backed bridge stress test is clean under TSan.

### ESP32 acceptance

- on-air ADV and scan-response bytes match the Milestone 0 fixtures;
- passive discovery by name/MSD and active discovery by service UUID both work;
- start-before-sync works after cold boot and deep-sleep wake;
- advertising resumes after an ordinary disconnect and a genuinely failed connection;
- it does not restart during a refresh hold or after teardown commits;
- repeated live MSD updates never show mixed revisions; and
- injected stop/deinit failure is visible to the application and logs.

### Status — 2026-08-05: steps 1-8 merged, EXIT GATE NOT MET

All eight merge-sequence steps are on `feat/f4-adv-control`:

| Step | Commit | Note |
|---|---|---|
| 1-2 controller + host tests | `dcf584e` | first source in `shared/`; 14 cases, 103 checks, mutation-checked |
| 3 target compiles it + ESP32 HAL | `841feb8` | the target had never consumed `shared/sources.cmake` at all |
| 4 identity snapshot | `33bda80` | event bridge deferred into step 5, where it has a reader |
| 5 loop owns advertising | `6d7b9c3` | `od_ble_advertise()`, `s_adv_wanted`, `s_msd` deleted |
| 6 packing via the HAL | (in `6d7b9c3`/`8e18139`) | needed no work once `od_ble_advertise()` was gone |
| 7 teardown barrier + F7 | `8e18139` | deinit reports; caller clears state only on success |
| 8 other-target adapters | this commit | recorded as import requirements, not stubs |

**The exit gate is NOT met, and F4/F7 are NOT closed.** It requires host *and hardware*
evidence; only the host half exists. Specifically missing:

- the Milestone 0 ADV/scan-response byte fixture, so there is **no evidence discovery bytes are
  unchanged**. The packing code is shared with the path it replaced and was not edited, which
  is an argument, not proof;
- every item in this milestone's ESP32 acceptance list — cold boot, deep-sleep wake,
  advertising across an EPD refresh, a genuinely failed connect, no restart after teardown
  commits, repeated MSD updates without mixed revisions;
- fault injection for the two failure paths F7 exists to report — `nimble_port_stop()` failing
  and GATT registration failing after a successful `nimble_port_init()`. Neither is reachable
  from a build.

Step 8 is deliberately documentation rather than code: `targets/nordic-zephyr/` and
`targets/efr32bg22-slc/` are one README each with no build system, so a "compile-tested fake"
would have nothing to compile it and would rot unnoticed. The link failure a missing
`od_hal_adv_*` produces is itself the guard.

## Milestone 2 — first protocol promotion: configuration

Promote the config TLV parser and chunked-write assembly as one tested subsystem. This closes
the known ESP32 `0x2A`/unknown-packet behavior while establishing the real shared-core vector
and fuzz workflow.

> **F3 CLOSES HERE — DEFERRED 2026-08-05.** An earlier revision pulled F3's `totalSize`
> enforcement forward into Milestone 0 as a ~30-line target-local fix. **That is reversed by
> decision:** F3 is not fixed ahead of the config promotion. It closes with `od_config.c`, in
> this milestone, behind Milestone 1.
>
> **What is exposed until then**, stated so the deferral is visible rather than implicit:
>
> - Malformed chunk sequences can commit a byte sequence inconsistent with the declared config
>   to NVS. Storage is changed before any downstream CRC check rejects it.
> - **On a device with encryption disabled the config-write path has no authentication gate at
>   all** (`configWriteGate()` returns ALLOWED when `!isEncryptionEnabled()`), so any BLE or LAN
>   client that can connect can reach it. With encryption enabled it needs either a session or
>   `REWRITE_ALLOWED`.
> - A 201-byte start frame is ACKed as an active transfer and never saved; a start frame longer
>   than 202 bytes silently discards the excess.
> - **F2 therefore delivers single-frame TLS config writes only.** Chunked TLS writes still
>   traverse the unbounded reassembly path, so "TLS clients can write config" remains
>   true-but-partial until this milestone.
>
> **This deferral has no review trigger yet.** D3 established that a kept deferral records an
> owner, an expiry, the affected versions, and the release that removes the exposure; the same
> applies here and none is set. Without them this rolls over silently — the exact failure D3
> exists to prevent, now applying to a High rather than to the BG22 pair.

### Required behavior

- validate declared total size at START before allocating or accepting context;
- require `received_size == total_size` before commit;
- reject excess bytes, missing bytes, duplicate final chunks, continuation without START, and
  connection changes mid-assembly;
- bind the assembly to the originating connection/session;
- never alter NVS on a rejected sequence;
- ~~parse every known canonical packet size even when the target does not implement the
  feature;~~ **DEFERRED (D4)** — incompatible wire change;
- ~~skip known-size unsupported packets and preserve later packets, especially security
  config;~~ **DEFERRED (D4).** Structure `od_config.c` so the skip model can be added later
  without re-cutting the parser, but do not implement it here and do not let a partial version
  arrive as a side effect. Current skip-to-CRC behaviour is preserved exactly;
- keep one shared scratch buffer within the BG22 budget; and
- make config-save session invalidation part of the shared contract.

### Test-first merge sequence

1. Replay the captured config corpus against the existing firmware and `py-opendisplay`.
2. Add authored boundary vectors for totals 0, 200, 201, 399, 400, 401, 4,000, 4,001, 4,096,
   and 4,097, interpreted according to the chunk-count decision.
3. Add truncated, reordered, duplicated, interleaved, disconnect, unknown-packet, and storage
   failure vectors.
4. Fuzz the pre-auth TLV parser and chunk state machine under ASan/UBSan.
5. Implement `od_config.c` against NVS test doubles and target-neutral apply callbacks.
6. Replace the ESP32 parser/assembly one slice at a time; do not batch storage, parser, and
   dispatcher changes into one unreviewable swap.

### Exit gate

- all config vectors pass against the C core and the compatible host surface;
- the parser is warning-clean C99 under GCC and Clang;
- malformed sequences demonstrably leave stored bytes unchanged;
- every supported `OD_*_ENABLE` permutation compiles;
- all ten ESP32 fragments build; and
- an S3 performs config write, reboot, and exact read-back using both single-frame and chunked
  paths.

## Milestone 3 — remaining ESP32 correctness closure

After config is isolated, finish the boundary defects without conflating them with target
migration.

### 3A — connection and origin policy: F5

- reject PIPE opcodes from LAN at the dispatcher boundary;
- implement the decided refuse-versus-evict behavior consistently for BLE and LAN;
- add tests proving a refused contender cannot perturb the incumbent transfer; and
- update the canonical protocol/host issue if the selected behavior differs from current text.

### 3B — lossless LAN egress: F6

- replace single short-write-prone sends with a queued complete-frame writer;
- preserve frame boundaries and ordering under lwIP and TLS backpressure;
- define bounded queue overflow and disconnect behavior; and
- fault-inject partial writes, retryable errors, peer closure, and permanent errors.

### 3C — WiFi event publication and mDNS: F9 and F10

- replace the `volatile` WiFi callback bundle with an ordered queue or coherent snapshot;
- make disconnect reason, BSSID/channel, retry state, and connection generation publish as one
  record where they are semantically coupled;
- implement actual trailing-edge MSD announcement coalescing with a 400 ms floor; and
- test cached-BSSID failure, low-RSSI roaming, event bursts, and repeated MSD changes.

### 3D — storage and error truth: F8

- propagate zero-write, commit, erase, and verification failures from secure erase;
- never log or return success when any required destructive step failed;
- inject NVS failures at every operation boundary; and
- verify raw stored bytes when the test environment permits it.

### Regression obligations for fixed F1/F2

The implementations are fixed on `main`, but the milestone is not complete without durable
coverage:

- at least 500 orderly LAN connect/close cycles with no descriptor growth; and
- TLS config single-frame write, chunked write, reboot, and exact read-back without an
  application-layer authentication session.

### Exit gate

F1-F10 each has either a tested fix or a versioned protocol/design decision. No item is closed
only because the observed happy path worked once.

## Milestone 4 — ESP32 system acceptance

This is a release decision, not a prerequisite to begin Milestones 1-3. Run it after the code
has stopped moving so the result describes a candidate that could actually ship.

### Automated matrix

- clean builds for all ten configured ESP32 boards;
- shim ratchet remains at or below 5;
- sdkconfig baselines and protocol-header sync checks pass;
- shared C suite passes with GCC and Clang under ASan/UBSan;
- concurrency models pass under TSan;
- protocol replay and config/advertising vector suites pass;
- standalone inflater differential tests pass where zlib development headers are available;
- LAN repeated-close and forced-short-write tests pass; and
- no temporary wake/connect instrumentation remains unless it is deliberately retained and
  documented.

### Hardware matrix

On an ESP32-S3 with WiFi enabled:

- BLE passive/active discovery, subscribe, authenticate, config, compressed push, uncompressed
  push, and disconnect/retry recovery;
- LAN plaintext and TLS discovery, config, compressed/uncompressed push, orderly reconnect, and
  BLE coexistence;
- config persistence across reboot;
- advertising restart and deep-sleep teardown while BLE/LAN events are arriving; and
- NVS and BLE-stop failures where a hardware-safe injection path exists.

On the rewritten panel paths:

- an IT8951/FastEPD panel visibly renders after the pixel-path fix; and
- at least one bb_epaper SPI panel and the parallel FastEPD path render and recover from an
  interrupted transfer.

Add C6 and other shipped-board smoke runs as hardware is available. Absence of a board must be
reported as `UNRUN`, never inferred from an S3 result.

### Accepted-debt format

Every unrun item must name:

- the exact path not exercised;
- why it is accepted;
- which release/target is exposed;
- the trigger that reopens it; and
- the owner responsible for arranging the missing hardware or test.

## Milestone 5 — Nordic/Zephyr nRF54L15 import

Once ESP32 correctness and the first two shared components are stable, resume the durable target
order in `MIGRATION.md`.

1. Pin NCS v3.3.1 explicitly; remove source-repo version globbing.
2. Import nRF54L15 sources with provenance and minimal logic changes.
3. Build and flash the imported target before replacing subsystems.
4. Implement the Zephyr advertising HAL/event bridge against `od_adv_control`.
5. Replace config with `od_config.c`, then dispatch, transfer paths, session, MSD build, PIPE,
   and compression one subsystem at a time.
6. Run host Gate 1 before each swap and target hardware Gate 2 after it.
7. Keep MCUboot configuration, settings preservation, and signed-image behavior from the donor.

Do not use Zephyr `k_work` as a new owner of product policy. It may bridge an API context, while
the application pump remains the owner of advertising, transfers, and teardown ordering.

## Milestone 6 — EFR32BG22 import

Import Silabs third, before the architecture can accidentally assume Zephyr-sized resources.

1. Use the pinned installed Simplicity SDK; do not import the 57 MB generated SDK copy.
2. Prove `slc generate` and a clean headless build against the installed SDK.
3. Import application sources and verify a hardware baseline before subsystem swaps.
4. Add the BGAPI advertising HAL while retaining superloop ownership—no synthetic RTOS layer.
5. Replace shared subsystems individually and measure RAM/flash after every swap.
6. Enforce the 4,096-byte config decision without duplicating scratch buffers.
7. Verify the `.gbl` OTA path and confirm any earlier security hotfix remains closed in the
   shared implementation.

Any shared API requiring a scheduler, heap allocation, blocking wait, or large automatic buffer
fails this milestone by design and must be corrected rather than hidden behind a Silabs fork.

## Milestone 7 — nRF52840 product decision and port

Before writing port code, decide which deployed units move:

- **Existing fleet stays on Bluefruit:** maintain it in the source repo and preserve host
  compatibility; only new production uses Zephyr/MCUboot.
- **Existing fleet migrates:** budget a physical bootloader replacement unless testing proves a
  Zephyr image can safely run under the current Adafruit/SoftDevice arrangement.

For units that move, make nRF52840 a board on `targets/nordic-zephyr/`, not a separate target.
Disable Bluefruit-style automatic advertising restart in the donor behavior and use the common
loop-owned controller. Only after this port removes the guarded nRF arms may the shim ratchet
reach zero and `targets/esp32-idf/compat/` be dismantled according to `SHIM_BUDGET`.

## Parallel work with deadlines

These tracks do not wait for the numbered milestones, but each has a point after which delay
becomes expensive.

| Track | Do now | Deadline |
|---|---|---|
| Wire corpus | Capture ESP32 first, then nRF54, Silabs, and nRF52840 sessions | Before the corresponding behavior is replaced by shared core |
| `py-opendisplay` defects | File the five verified issues; prioritize timed-sleep `0x0052` and permanent auth retry | Before the next HA/host release decision |
| Protocol synchronization | Add this repo to the copy map and enforce `--check` in CI. **Now a prerequisite, not parallel work** — D4 commits to `MAX_CONFIG_CHUNKS` 20 → 21, and until the map is wired a local bump diverges from canonical undetectably | **Before** the chunk-count bump, not merely before the release |
| Canonical `MAX_CONFIG_CHUNKS` | Raise 20 → 21 upstream in `../opendisplay-protocol` (D4, confirmed 2026-08-05), then propagate with `--push` | Blocked by the header freeze; raise the issue/PR now so it lands the moment the freeze lifts |
| Canonical eviction text | **Correct LAN rule (3) upstream in `../opendisplay-protocol`: refusal, not eviction** (D4, confirmed 2026-08-05). Blocked by the header freeze, so raise it as an issue/PR now and land it when the freeze lifts | Before any host is written to the current text — every day it stands, a conforming client may be built expecting eviction |
| Three-toolchain CI | Design reproducible IDF/NCS/Simplicity builds, not merely a local matrix | Before the second target is called imported |
| BG22 security | Hotfix or explicitly re-accept with owner/expiry | During Milestone 0 |
| OTA | Scope ESP32 `esp_ota` and host SMP/mcumgr support | Before promising field updates for S3 or Nordic |
| `s3-e1004` | Re-vendor the correct panel fork and fix SPI initialization | Before claiming all source ESP32 variants are supported |

## Immediate work queue

If work starts from this document today, the next five implementation units are:

1. Commit the Milestone 0 advertising byte fixture and the decisions needed by F4.
2. Commit failing host tests for `od_adv_control` with a fake HAL.
3. Commit the shared controller and make the ESP32/host builds consume it.
4. Commit the ESP32 event bridge, identity snapshot, and removal of callback-owned advertising
   policy.
5. Commit the teardown/error propagation and run the focused F4 hardware matrix.

Only then begin the config corpus/fuzz/implementation commits in Milestone 2.

## Change discipline

- One subsystem per reviewable commit; do not batch target migration swaps.
- Tests lead each shared implementation rather than following it.
- Preserve user-visible wire behavior unless a canonical decision explicitly changes it.
- Keep target AD packing, stack calls, storage drivers, and synchronization primitives in
  `targets/`; keep policy/state machines in plain C under `shared/`.
- Update the divergence matrix when choosing between donor behaviors.
- Update this document when a milestone changes state; move completed detail to a dated record
  instead of allowing the live sequence to become another history log.
- Never call an unrun test passed. Use `ACCEPTED-UNRUN` when the project consciously proceeds
  without it.

## Definition of completion for this plan

This plan is not complete when ESP32 merely builds. It is complete when:

- F1-F10 have tested fixes or explicit versioned decisions;
- F4 advertising/lifecycle control and config parsing are shared, portable C with host tests;
- the ESP32 release matrix has a recorded result for every item, including accepted debt;
- nRF54L15 and EFR32BG22 consume the shared components without target forks;
- the deployed nRF52840 split is decided and the chosen population has a maintained path;
- protocol synchronization and three-toolchain CI are enforced; and
- the remaining Arduino shim count reaches zero only through the verified nRF migration, after
  which the temporary compatibility directory and ratchet are retired as documented.
