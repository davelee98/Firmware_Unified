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
4. ~~Close the remaining ESP32 correctness findings in transport, event handoff, lifecycle
   truth, secure erase, and mDNS.~~ **ALL F1-F10 CLOSED 2026-08-05.** Three fixed, two closed
   on merged code without hardware evidence, one partly upstream, four WONT-FIX. See
   Milestone 3 for the disposition table and what each closure does not change.
5. Run the release acceptance matrix and explicitly record any hardware debt still accepted.
6. Import nRF54L15 into `targets/nordic-zephyr/`, one subsystem at a time.
7. Import EFR32BG22, using its no-kernel and 32 KB RAM limits as hard shared-core gates.
8. ~~Decide the deployed nRF52840 product split~~ — **DECIDED 2026-08-05: it migrates.** Port
   nRF52840 as a board of `targets/nordic-zephyr/`. Prerequisite: the host SMP/mcumgr client.

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
| Correctness review | **ALL F1-F10 CLOSED 2026-08-05.** F1/F2/F3 fixed; F4/F7 closed on merged code with NO hardware evidence; F5 partly upstream; F6/F8/F9/F10 WONT-FIX | The review is discharged as a tracking artifact. That is not the same as the target being correctness-signed-off — see Milestone 3's disposition table |
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

**The consequence to keep in view:** F3 is High, and this ordering put its closure in Milestone
2, behind a Medium. Pulling F3's fix forward was considered and **rejected by decision on
2026-08-05** — it was to close with the config promotion, and it did: **F3 is FIXED** in
`96a29b8`. The deferral this paragraph previously described as having no review trigger is
therefore discharged, not running. Milestone 2 records how.

`od_config.c` is still first among subsystems that parse, store, or alter wire behavior. The
config-first security rationale in the historical plan therefore remains intact.

### D2 — old hardware gaps are accepted debt, not prerequisites — **CONFIRMED 2026-08-05**

The uncompressed-push and interrupted-recovery runs do not block starting F4. They remain in the
system acceptance matrix and must be reported as `PASS`, `FAIL`, or `ACCEPTED-UNRUN`; they must
not disappear merely because the old item was closed.

Confirmed as accepted debt. The two paths stay named in Milestone 4's hardware matrix and are
reported `ACCEPTED-UNRUN` until someone runs them — not omitted, and not inferred from the
compressed-push result that did pass.

### D3 — BG22 security defects — **CLOSED 2026-08-05: resolved by the shared promotion**

**Decision: closed. These are fixed when the session/dispatch code becomes shared, not before,
and not separately.** The earlier deferral -- with four UNSET review-trigger fields -- is
superseded by this closure. There is nothing left tracking here.

WHY THIS IS A CLOSURE AND NOT A DEFERRAL. A deferral needs an expiry because nothing else makes
it come back. This has a mechanism instead: `shared/core` replaces the Silabs session and
dispatch implementations, and **the shared implementations cannot land carrying these defects**
-- the shared dispatcher gates on `sec_enabled()` and clears the session on config save, because
that is what the other two targets already do and what the divergence matrix already selects as
the winning behaviour. Fixing them is not additional work scheduled for later; it is a property
of the promotion itself.

That mechanism is already written down and is not a promise made here:

- **`DIVERGENCE_MATRIX.md` §1.5a** -- the `<31 byte` bypass. Resolution recorded as: *"shared
  dispatcher must gate on `sec_enabled()`, never on frame length."* NRF54's mid-session
  plaintext rejection is named as the correct model.
- **`DIVERGENCE_MATRIX.md` §2.4** -- session survives a key change. Resolution recorded as:
  *"Firmware/NRF54 behaviour wins"* -- `clear_session()` on every save path.
- **`MIGRATION.md` § "Risks to watch"** already requires these be **Gate 1 test cases, not
  TODOs**, so the promotion cannot pass its own gate while they stand.

WHAT REMAINS TRUE IN THE FIELD UNTIL THEN, recorded because closing a tracking item does not
change a shipped device:

- `opendisplay_pipe.c:1238` gates authentication on `frame_len >= 31u`. A shorter frame skips
  the session check and the decrypt entirely and reaches `dispatch()` on line 1251. REBOOT,
  DEEP_SLEEP, LED, BUZZER and CONFIG_READ are all short enough.
- No `clear_session()` on any config-save path, so rotating the encryption key does not evict a
  live session.
- BG22 is the one fleet with a working `.gbl` field-update path, so whenever the shared code
  lands it reaches deployed units without a bench visit. That is the same fact that made the
  deferral uncomfortable and it is what makes this closure workable.

**ONE DISCREPANCY FOR WHOEVER WRITES THE GATE 1 TEST.** §1.5a characterises the exposure as
requiring a live session -- *"once any client authenticates"* -- because `dispatch()` only blocks
when there is no session. The code at `:1238-1251` appears broader than that: for
`frame_len < 31` the `if` is false, so the session check never runs at all and the frame
dispatches with or without a session. Write the test for the broader reading and let it decide
which is right; if the broader reading holds, §1.5a understates it.

Milestone 0's exit gate item *"the BG22 security choice has an owner and deadline"* is satisfied
by this closure rather than by filling the fields: the choice is made and its removal is bound to
migration step 3 rather than to a date.

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
4. ~~Re-take the BG22 security-hotfix decision~~ — **DONE 2026-08-05: CLOSED** (D3). Resolved
   by the shared session/dispatch promotion, which cannot land carrying these defects
   (DIVERGENCE_MATRIX §1.5a, §2.4 are Gate 1 test cases). No owner/expiry needed — the removal
   is bound to migration step 3, not to a date.
5. ~~Decide the 20-versus-21 chunk count~~ — **DONE 2026-08-05: it becomes 21** (D4). What
   remains for this gate: state the interim **4,000-byte** ceiling in tests and host-facing
   documentation, and **add `shared/protocol/` to the sync tool's copy map before any bump**,
   since nothing currently detects drift between this vendored copy and canonical.
6. ~~Land F3's `totalSize` enforcement as a target-local fix~~ — **MOOT: F3 IS FIXED**
   (2026-08-05, `96a29b8`). It was deferred to Milestone 2 rather than pulled forward, and
   Milestone 2's first slice then closed it. Nothing is owed here.
7. ~~Propagate D1's promotion order into `CLAUDE.md` and `MIGRATION.md`~~ — **DONE 2026-08-05**
   (commit `5cf6ded`). Both now name `od_adv_control.c` as the first `shared/` source, and
   `CLAUDE.md` points at this document as the live sequence.

### Exit gate

- the ADV/scan-response byte fixture exists;
- the corpus schema is fixed and non-empty config captures exist;
- every D4 item has one named behavior rather than two competing descriptions; and
- ~~the BG22 security choice has an owner and deadline~~ — **MET 2026-08-05 by closure**: D3 is
  closed as resolved-by-promotion, so the removal is bound to migration step 3 rather than to a
  date. No owner or expiry is outstanding.

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

### Status — 2026-08-05: steps 1-8 merged; F4/F7 CLOSED, NOT VERIFIED

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

**F4 and F7 are CLOSED by decision, on merged code — not verified.** The eight merge steps are
done and the findings are discharged as tracking items (Milestone 3's disposition table records
them as *CLOSED (code merged, evidence outstanding)*). Closing them did not produce the evidence
this milestone's exit gate asked for: it requires host *and hardware* results, and only the host
half exists. Still missing, unchanged by the closure:

- the Milestone 0 ADV/scan-response byte fixture, so there is **no evidence discovery bytes are
  unchanged**. The packing code is shared with the path it replaced and was not edited, which
  is an argument, not proof;
- every item in this milestone's ESP32 acceptance list — cold boot, deep-sleep wake,
  advertising across an EPD refresh, a genuinely failed connect, no restart after teardown
  commits, repeated MSD updates without mixed revisions;
- fault injection for the two failure paths F7 exists to report — `nimble_port_stop()` failing
  and GATT registration failing after a successful `nimble_port_init()`. Neither is reachable
  from a build.

Read "closed" as a tracking state and nothing else. It does not mean verified, and the list above
does not shrink because the findings were closed — the evidence remains owed, and comes due in
Milestone 4's hardware matrix.

Step 8 is deliberately documentation rather than code: `targets/nordic-zephyr/` and
`targets/efr32bg22-slc/` are one README each with no build system, so a "compile-tested fake"
would have nothing to compile it and would rot unnoticed. The link failure a missing
`od_hal_adv_*` produces is itself the guard.

## Milestone 2 — first protocol promotion: configuration

Promote the config TLV parser and chunked-write assembly as one tested subsystem. This closes
the known ESP32 `0x2A`/unknown-packet behavior while establishing the real shared-core vector
and fuzz workflow.

> **F3 IS CLOSED — fixed 2026-08-05 in `96a29b8`,** by the first slice of this milestone.
> `shared/core/od_config_asm.c` enforces the declared `totalSize` and commits on an exact byte
> count rather than a chunk count, with 101 host checks over the review's boundary matrix.
>
> **THE DEFERRAL RECORDED HERE IS SUPERSEDED, not still running.** An earlier revision of this
> document deferred F3 with four `UNSET` review-trigger fields and warned it would roll over
> silently. That warning is discharged: it did not roll over, because the milestone that owned
> it closed it. The exposure it described — malformed input committing an inconsistent record
> to NVS, reachable without authentication on a device with encryption disabled — no longer
> exists on `main` once this branch merges.
>
> One consequence to carry forward: F2 delivered single-frame TLS config writes only while this
> was open. With reassembly fixed, chunked TLS writes are now correct **by construction**, but
> the end-to-end path is still unproven on hardware — a real chunked write over BLE and TLS,
> reboot, and exact read-back remain owed by Milestone 4.

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

## Milestone 3 — remaining ESP32 correctness closure — **ALL FINDINGS CLOSED 2026-08-05**

**Every finding from CORRECTNESS_REVIEW_2026-08-04.md is now closed.** The review is discharged
as a tracking artifact; it remains valuable as the record of what was found and why.

CLOSED IS NOT ONE STATE, and flattening these into "done" would destroy the only information
worth keeping. Four dispositions are used below and they mean different things:

| | Meaning |
|---|---|
| **FIXED** | code merged on `main`, with tests |
| **CLOSED (code merged, evidence outstanding)** | the fix is on `main`; the review's own exit gate additionally required hardware evidence, which does not exist |
| **CLOSED (WONT-FIX)** | behaviour accepted as-is; no fix planned |
| **CLOSED (partly upstream)** | the part this repo owns is done; a canonical-header change remains, tracked in parallel work |

### Disposition

| # | Sev | Disposition | Where |
|---|---|---|---|
| F1 | High | **FIXED** — orderly LAN close no longer leaks the accepted socket | `2a1cd96` |
| F2 | High | **FIXED** — TLS-authenticated LAN clients can write config | `b69fb04` |
| F3 | High | **FIXED** — chunked reassembly enforces the declared `totalSize`; commits on an exact byte count | `96a29b8` (`shared/core/od_config_asm.c`) |
| F4 | Med | **CLOSED (code merged, evidence outstanding)** — the loop owns advertising; callbacks publish facts | `6d7b9c3`, `33bda80`, `dcf584e` |
| F5 | Med | **CLOSED (partly upstream)** — PIPE rejected on LAN | `1cfe78b` |
| F6 | Low* | **CLOSED (WONT-FIX)** — LAN writes still not retried to completion | — |
| F7 | Med | **CLOSED (code merged, evidence outstanding)** — deinit reports failure; caller clears state only on success | `8e18139` |
| F8 | — | **CLOSED (WONT-FIX)** — secure-erase reporting accepted as-is | — |
| F9 | Med | **CLOSED (WONT-FIX)** — WiFi event handoff still uses `volatile` | — |
| F10 | Low | **CLOSED (WONT-FIX)** — the mDNS 400 ms floor stays inverted | — |

\* F6 was rated Medium by the review. Verification showed the accepted socket is blocking and
the oversized two-write fallback is unreachable at current buffer sizes (642 B buffer vs a 600 B
maximum response), so the live risk is lower. It reverts to Medium if either fact changes.

### What closing these does NOT change

Closing a finding changes the tracking state, not the firmware. Recorded once, factually, so it
is discoverable without re-reading the review:

- **F4 / F7 have no hardware evidence.** The review's exit gate required host *and* hardware
  results. Only host results exist: no ADV/scan-response byte fixture, so there is no proof
  discovery bytes are unchanged; none of the ESP32 acceptance list has run; and the two failure
  paths F7 exists to report — `nimble_port_stop()` failing, and GATT registration failing after
  a successful `nimble_port_init()` — are not reachable from a build at all. These are closed on
  merged code, not on evidence.
- **F5's canonical contradiction stands.** The header still says a new LAN client EVICTS the
  incumbent while this firmware refuses it. Settled in favour of refusal (D4); the fix is an
  upstream `opendisplay-protocol` change, tracked in the parallel-work table. The
  `OD_ERR_PIPE_START_WRONG_TRANSPORT` code F5's NACK wants is tracked there too.
- **F6** — LAN response writes are not retried to completion.
- **F9** — the WiFi event→loop handoff uses `volatile`, which is not synchronisation. Paired
  scalar-then-flag writes have no ordering, so the loop can act on a pending flag with a stale
  reason/channel/RSSI, and an unsynchronised `usingCachedAp` can miss the full-scan fallback
  after a cached BSSID fails.
- **F10** — a *changed* mDNS payload bypasses the 400 ms floor entirely and an *unchanged* one
  re-announces once it elapses: the inverse of the stated policy, permitting multicast bursts on
  a battery device sharing its radio with BLE.
- **F8** — see the entry retained below; the AES master key may remain recoverable on a failure
  path while the log reports success.

### F8 — the reasoning, retained

The context that makes F8's disposition defensible, kept because it is the least obvious of the
four: `od_hal_nvs.h`'s contract already states the overwrite is **best-effort** on a
log-structured store. NVS may write the zero blob to a fresh page and leave the original intact
whatever any call returns. A fix would make the *reporting* truthful, not the erase guaranteed —
that needs whole-partition destruction, a separate and larger decision. **Revisit F8 if that
decision is ever taken**, since truthful reporting is worth more once there is something true to
report.

### Regression coverage that was never written

Named because closing F1/F2/F3 does not conjure it: the review asked for at least 500 orderly
LAN connect/close cycles with no descriptor growth (F1), and a TLS config single-frame write,
chunked write, reboot and exact read-back without an app-layer session (F2). Neither exists.
F3's boundary matrix DOES exist and passes — 101 host checks, mutation-verified.

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

## Milestone 7 — nRF52840 port — **DECIDED 2026-08-05: it migrates**

**The nRF52840 migrates to `targets/nordic-zephyr/` as a board.** The product question Milestone
7 opened -- migrate the fleet, or leave deployed units on Arduino/Bluefruit indefinitely while
only new production ships Zephyr -- is answered: it migrates.

### Bootloader: TWO TIERS, decided 2026-08-05

**nRF52840 keeps the Adafruit bootloader. nRF54L15 / nRF54LM20A use MCUboot.** One target, two
bootloaders, deliberately -- not an inconsistency to tidy up later.

This REVERSES two consequences recorded earlier the same day, and the reversal is the point:

| Earlier note | Now |
|---|---|
| "Deployed units need their bootloader replaced, which needs PHYSICAL ACCESS" | **No physical access.** They keep the bootloader they have. |
| "The host SMP/mcumgr client is a PREREQUISITE" | **Not for nRF52840.** It keeps its existing Nordic DFU path, which `py-opendisplay`'s `perform_nrf_dfu` already drives. The SMP client is needed only for the nRF54 boards. |

What made the earlier framing wrong was assuming Zephyr implies MCUboot. It does not. Evidence
gathered before deciding:

- **NCS ships `xiao_ble/nrf52840` upstream** with `pm_static.yml` for the ADAFRUIT layout, not
  MCUboot partitions. The configuration that needs no physical access is the one upstream
  already supports; MCUboot on this board means writing partitions by hand.
- **Zephyr under the Adafruit bootloader is a shipped configuration elsewhere** (ZMK on
  nice!nano: nRF52840, Adafruit bootloader, S140 resident but never enabled, Zephyr's own
  controller owning the radio). Not speculative.
- Attempting the build confirmed the first failure is `slot0_partition` missing -- an MCUboot
  requirement, not an SoC problem.

**What nRF52840 gives up, accepted:** no signed images and no automatic revert. Those units keep
exactly the update story they have today, which is a working one. New production on nRF54 gets
the stronger story. A two-tier fleet is the deliberate outcome, not drift.

**What still needs testing** -- unchanged, and now on the critical path rather than a
cost-avoidance check: build a Zephyr image at the Adafruit flash layout, push it to ONE unit over
the existing BLE DFU, confirm it boots and stays up. The precedent says it works; the precedent
is not this firmware.

### What else follows

- **The Arduino shim can finally die.** `targets/esp32-idf/compat/` sits at its floor of 5, and
  those files are counted only for `TARGET_NRF` arms. They leave with this port; only then does
  the ratchet reach zero and `compat/` get dismantled per `SHIM_BUDGET`.
- **The advertising divergence closes.** Set `restartOnDisconnect(false)` and adopt
  `od_adv_control`; that deletes `restartsAdvertisingOnDisconnect()` and its special-case branch
  in `serviceBleAdvertisingRestart()`, because Bluefruit's automatic restart is a second policy
  owner.
- **`zephyr/sysbuild.conf` must become board-conditional.** It currently hard-sets
  `SB_CONFIG_BOOTLOADER_MCUBOOT=y` for every board, which is what makes an nRF52840 configure
  fail today.

### Scope of the port, measured rather than estimated

- **No custom board definition needed** -- `xiao_ble/nrf52840` is upstream. (The plain variant:
  the physical board is NOT the Sense, despite `Firmware/boards/nrf52840custom.json` saying
  `usb_product: "XIAO nRF52840 Sense"`. That field is wrong.)
- **The GPIO layer is already portable.** `nrf54_gpio.c` uses Zephyr's generic API
  (`gpio_pin_configure`, `DEVICE_DT_GET`), not nRF54 registers, so the 14 of 26 source files
  that include it come across unchanged. Only the name is SoC-specific.
- **The real work is a board-specific devicetree overlay.** `zephyr/app.overlay` is written
  against nRF54L15 node labels (`spi00`, `uart20`, `i2c22`); nRF52840 has `spi0..spi3`. Normal
  per-board Zephyr work, not a port.
- **One small fix:** `gpio_dev()` guards `gpio3` behind `DT_NODE_HAS_STATUS` but not `gpio2`.
  nRF52840 has neither.

### Order

Still last. It depends on `targets/nordic-zephyr/` existing AND being verified on hardware
(Milestone 5 step 3), which has not happened. Nothing is blocked *by* this decision; it unblocks
planning, not code.

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
| ~~BG22 security~~ | **CLOSED 2026-08-05** (D3) — resolved by the shared session/dispatch promotion; DIVERGENCE_MATRIX §1.5a and §2.4 are Gate 1 test cases, so the promotion cannot pass carrying them | — |
| OTA | Scope ESP32 `esp_ota` and host SMP/mcumgr support | Before promising field updates for S3 or Nordic |
| ~~`s3-e1004`~~ | **RETIRED 2026-08-05** — the board variant is retired, so no fragment is wanted. The blocker also dissolved: upstream merged E1004 support at `5dccfbb` and the `BBEP_T133A01` stream it needed fixing no longer exists | — |

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
