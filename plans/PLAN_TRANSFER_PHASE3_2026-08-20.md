# Transfer Phase 3 — promote PIPE

**Date:** 2026-08-20

**Source snapshot:** `main` at `c58e23f` (post shared-logging promotion and merged Nordic SPIM
implementation). The SPIM hardware rows remain an entry gate; merge is not qualification.

**Status:** software candidate implemented on `codex/transfer-phase3` on 2026-08-20 under the
§ 6 sequencing exception. All hardware evidence rows remain open; this status is not Phase 3
hardware qualification.

**Authority:** this document owns every Phase 3 decision, staging step, test obligation, cutover
inventory transition and gate. `plans/PLAN_TRANSFER_PROMOTION_2026-08-17.md` remains the record of
the whole transfer sequence and still owns Phases 4 (NFC) and 5 (cleanup/release), the § 4 wire
behaviour freeze, the § 5 architecture rules and the § 7-§ 11 cross-phase gates. Where the master
plan's own Phase 3 section and this document disagree, **this document wins**; the master section
is marked superseded and points here.

---

## 1. Outcome

One PIPE state machine, in `shared/core/od_pipe.{c,h}`, owning `0x0080`, `0x0081` and `0x0082`
for every target:

- `OD_DISPATCH_OPCODE_ROWS` names `od_pipe_start()`, `od_pipe_data()` and `od_pipe_end()`
  directly, with the reservation budgets `1`/`3`/`3` unchanged.
- The three `od_cmd_app_pipe_*` declarations leave `shared/core/od_cmd_app.h`, and every target
  definition of them is deleted.
- ESP32's and Nordic's target PIPE machines — parser, reorder queue, SACK builder, ack cadence,
  NACK construction, completion policy, per-target byte counters — are deleted, together with the
  Phase 2 step-10b inventories the master plan froze.
- BG22, at `OD_CAP_PIPE=0`, links the shared entry points, emits its existing refusal bytes and
  allocates no reorder slot, no sequence state and no PIPE branch it cannot reach.
- `od_core_reset()` owns transfer reset; targets stop calling `od_xfer_reset()` and
  `opendisplay_pipe_write_reset()` from their disconnect paths.
- One repository-wide production ratchet: only `shared/core/od_xfer.c` resets or pushes
  `od_zlib_pump`.

Phase 3 is a **move plus the frozen decisions in § 4**, not a redesign. Every byte on the wire is
either preserved exactly or changed by a decision named in § 4 with its own test and its own
hardware row.

---

## 2. Entry preconditions — all blocking

Phase 3 deletes the only reference implementations of PIPE that exist. Nothing here may be
started before all of the following hold. This is stricter than the master plan's boundary,
because two Phase 2 cutovers were implemented ahead of their hardware evidence by project
direction and that debt is now the gate.

### 2.1 Nordic panel SPIM lands and is hardware-qualified

`plans/PLAN_NORDIC_SPIM_8MHZ_2026-08-19.md` changes the Nordic panel byte loop that every PIPE
DATA frame ultimately feeds, on all three boards. Its implementation is merged at this plan's
source snapshot, but its hardware rows remain open. Running the PIPE promotion against an
unqualified panel write path makes any Nordic PIPE failure ambiguous between the two changes.
SPIM's own hardware rows must be closed first.

### 2.2 Transfer Phase 2 hardware qualification, per target

Every unchecked row under `docs/HARDWARE_VERIFICATION_CHECKLIST.md`
§ "Transfer Phase 2 — ESP32 steps 10a/10b", § "Transfer Phase 2 — Nordic steps 10a/10b" and
§ "Transfer Phase 2 — EFR32BG22 step 10a" must be closed or explicitly recorded as unavailable
with the board named. In particular the **bidirectional-replacement row on each PIPE-capable
target** — PIPE START displacing a live `od_xfer`, legacy START displacing a live target PIPE,
the displaced owner's DATA/END inert, a fresh transfer then succeeding — is the row that proves
the interim two-machine state behaved before Phase 3 collapses it into one.

A build is not a pass. Implementation-by-direction is not a pass.

### 2.3 Pre-promotion PIPE reference rows on `xiao_nrf52840`

These lose their reference value the moment the target machine is deleted, and are cheap now:

- plaintext (unencrypted) PIPE upload;
- PIPE-partial (`0x0080` flags bit 1) end-to-end — accepted START, region streamed, partial
  refresh, new etag committed. This path was refused on every attempt until the
  2026-08-19 flag-domain fix (`docs/FOLLOWUPS.md` § 6); it is host-tested and has never run on a
  board, so PIPE-partial has **no** hardware precedent at all;
- `OD-S1` replay injection through `dispatch-gate`, which is the only stimulus that exercises the
  silence-on-nonce-rejected-`0x81` rule Phase 3 must carry forward.

### 2.4 The small-tail stall is reproduced or retired, with a transcript

`docs/HARDWARE_VERIFICATION_CHECKLIST.md` records small/sub-cadence PIPE tails stalling
indefinitely, diagnosed 2026-08-17 on Nordic and asserted identical on ESP32
(`display_service.cpp:2780`). Current Nordic code SACKs an already-consumed duplicate immediately
when no gap is open, and the Python sender performs exactly that duplicate probe — so the claim
and the code disagree.

Reproduce it on current HEAD with a raw transcript, or retire it as stale in the checklist with
the evidence that retires it. Do **not** carry the claim into Phase 3 unexamined, and do not add a
flush timer without a failing trace (§ 4, D11).

### 2.5 The compression-admission divergence is decided [resolved by D6]

`docs/FOLLOWUPS.md` § 3.10: after Phase 2, a compressed legacy `0x70` is accepted on Nordic
regardless of the config's `TRANSMISSION_MODE_STREAMING_DECOMPRESSION` bit, while a compressed
PIPE upload of the same image is still refused, because target PIPE funnels through
`opendisplay_display_direct_write_start()`. Shared PIPE cannot inherit a target config gate, so
the answer must be frozen before the machine is written. See D6.

**Exit of § 2 is a written statement in `docs/HARDWARE_VERIFICATION_CHECKLIST.md`**, per target,
recording which rows passed, which are unavailable, and on which board and date.

---

## 3. Ground truth at authoring

### 3.1 Where PIPE lives today

| Target | PIPE implementation | Command hooks | Lines |
|---|---|---|---:|
| ESP32-IDF | `src/display_service.cpp` (`handlePipeWrite*`, `pipeState`, `pipeReorder`) | `src/od_cmd_app.cpp`, behind `pipe_refused_on_lan()` | ~600 of 3,351 |
| Nordic/Zephyr | `src/opendisplay_pipe_write.cpp` | same file, tail | 605 |
| EFR32BG22 | none | `od_cmd_silabs.c`, explicit refusal | ~14 |

Both machines are structurally the same code: `PipeWriteState` + a 33-slot `PipeReorderSlot`
array, `pipe_slot()`, `pipe_chunk_received()`, `pipe_build_ack_payload()`,
`pipe_update_highest_seen()`, cadence and gap counters, a fatal-error latch, and a
finalize/refresh tail. The differences are enumerated in § 4 and are exactly what Phase 3 has to
decide rather than discover.

### 3.2 What already exists shared, and is reused unchanged

- `od_xfer_state_t` (`shared/core/od_xfer_internal.h`) already reserves `OD_XFER_PIPE_FULL`,
  `OD_XFER_PIPE_PARTIAL` and `OD_XFER_FATAL`, and already holds `owner`, `started_ms`,
  `expected_bytes`, `received_bytes`, `written_bytes`, `compressed`, `geometry` and the
  `#if OD_CAP_PARTIAL` partial rectangle/etag block.
- `od_xfer_stream_reset()` / `od_xfer_stream_push()` are the single inflater driver, and
  `stream_sink()` is the single output accounting site.
- `od_xfer_reply_app()` (sealed, via `od_reply()`) and `od_xfer_reply_error()` /
  `od_xfer_reply_simple_error()` (plaintext) are the single seal-or-plain choice sites.
- `od_xfer_app.h` is complete and needs **no new function** for PIPE: `begin_full`,
  `begin_partial`, `write`, `inflate_scratch`, `abort`, `before_refresh`, `barrier_abort`,
  `refresh`, `displayed_etag`, `set_displayed_etag`, `now_ms` and `panel_info` cover it.
- `shared/sources.cmake` tier `APP_XFER` already carries `od_xfer.c`, `od_xfer_direct.c` and
  `od_xfer_partial.c`, and **every** `APP_SESSION` consumer takes `APP_XFER` — so BG22 links this
  tier and `od_pipe.c` will land there.
- `od_core.c` is in `APP_SESSION`, whose consumers all take `APP_XFER`, so `od_core_reset()` may
  call `od_xfer_reset()` with **no tier change** (step 7).

### 3.3 Sizing facts

| Fact | ESP32 (normal) | ESP32 `PIPE_SMALL_DRAM_WINDOW` | Nordic | BG22 |
|---|---:|---:|---:|---:|
| `PIPE_MAX_W` / `PIPE_MAX_N` | 32 / 32 | 16 / 16 | 32 / 32 | n/a |
| `PIPE_REORDER_SLOTS` | 33 | 17 | 33 | n/a |
| `PIPE_REORDER_SLOT_SIZE` | 248 | 248 | 248 | n/a |
| Reorder `.bss` | 8,184 B | 4,216 B | 8,184 B | 0 |

`PIPE_MAX_FRAME` is 244 and `PIPE_FRAME_OVERHEAD` is 3 in the canonical header, so the largest
legitimate plaintext DATA payload is 241 bytes; the deployed slot width of 248 is 7 bytes of slack
per slot. `PIPE_SMALL_DRAM_WINDOW` exists because the classic-ESP32 `esp32-N4` and
`esp32-wrover-e-N4R8` envs overflow `dram0_0_seg` by ~672 B at 33×248.

The `OD_RXQ_SLOTS >= PIPE_MAX_W + 2` and `OD_TXQ_SLOTS >= PIPE_MAX_W + 2` assertions live in
`targets/esp32-idf/src/structs.h` and `targets/nordic-zephyr/src/opendisplay_pipe_write.cpp`
today, each stating that `shared/` cannot see `PIPE_MAX_W`. After D5 it can.

---

## 4. Frozen decisions

Each of these is wire- or session-visible, is decided here rather than during implementation, and
carries the test and (where behaviour changes) hardware obligation named with it.

### D1 — Command verdicts are truthful; DATA silence and END NACK are distinct

**Decision.** Adopt C11's verdict rule on both targets: a frame that accepts nothing returns
`OD_CMD_NACK`; a frame genuinely absorbed — including a duplicate that is discarded and SACKed —
returns `OD_CMD_OK`. The verdict does **not** by itself choose silence. Preserve the deployed
per-opcode reply policy:

- inactive DATA, DATA after the fatal latch, zero-length DATA and wrong-owner DATA emit nothing;
- inactive END and fatal-state END emit the plaintext two-byte `{FF,82}` NACK. Inactive wins over
  ownership: when no transfer is open, an END from any `{origin, tag}` takes this arm;
- wrong-owner END emits nothing, so one connection cannot solicit a response about another
  connection's transfer;
- a genuinely absorbed duplicate emits its SACK.

Every refusal above returns `OD_CMD_NACK`. Under the current `od_frame_policy()` table,
`OD_FRAME_HANDLER_NACK` and `OD_FRAME_ACCEPTED` both stamp activity, reset the auth-abuse run and
consume RX. Phase 3 deliberately preserves that policy: a silently discarded PIPE DATA frame
continues to hold the exclusive link open. Making silent PIPE loss non-stamping would require a
new outcome (with `OD_FRAME_CRYPTO_DROPPED` as the precedent), its own policy decision and its own
tests; it is not smuggled into this verdict cleanup.

**Why it matters.** The verdict truthfully records whether the handler absorbed the frame and keeps
telemetry/future policy changes from treating a silent discard as accepted work. It does **not**
change activity today: `OD_CMD_OK` and `OD_CMD_NACK` map to policy rows with identical effects.
Reply choice remains opcode-specific. A `0x81` hard NACK is fatal to a client's upload loop, so
inactive DATA stays silent. Both deployed machines answer inactive/fatal END with `{FF,82}`, so
that byte-visible behaviour stays intact.

**Divergence resolved.** ESP32 currently answers `OD_CMD_OK` to inactive, fatal-state,
zero-length and wrong-owner DATA (`display_service.cpp:2703-2705`); Nordic already answers
`OD_CMD_NACK`. Nordic's form is adopted — this is the one place the general "`Firmware` is the
authority" default is overridden, because C11 already decided the verdict question repository-wide
and ESP32's `OD_CMD_OK` predates it.

### D2 — START ordering: replacement first, ACK before hardware, unwind on substitution

**Decision.** The START sequence is fixed as:

1. reject a non-BLE origin through D8, without touching any transfer state;
2. on a PIPE-capable BLE target, displace any live transfer **before parsing the START body**;
3. parse and validate (including D4's frame floor and D6's compression admission);
4. arm shared `od_xfer` state under `OD_XFER_PIPE_FULL` / `OD_XFER_PIPE_PARTIAL`;
5. queue the START ACK through `od_xfer_reply_app()`;
6. if that reply was substituted, **unwind the armed state** — abort through
   `od_xfer_app_abort(OD_XFER_ABORT_REPLY_FAILED)`, clear the displayed etag on a partial, reset
   PIPE sequencing — and emit nothing further;
7. only then activate hardware (`od_xfer_app_begin_full()` / `od_xfer_app_begin_partial()`);
8. a **post-ACK** activation failure enters the fatal state (D3) and emits **no** contradictory
   START NACK — the first DATA is silently refused and the host retires the attempt through its
   existing retransmission/timeout policy.

Step 2 preserves both deployed machines and the canonical rule that a new START aborts an
in-flight transfer: a malformed, unsupported or size-mismatched BLE START still displaces the old
owner. Capability-off BG22 and D8's LAN refusal are inert exceptions because they do not implement
or admit PIPE.

**Why.** This is deliberately the opposite of the legacy `0x70` order, and it is load-bearing:
Spectra/ACeP-class bring-up runs for seconds while the client gates its `0x0080` wait on an
ordinary command timeout. Recorded as a permanent divergence from the master plan's § 5.3 END
sequence; the two are not normalised.

**Divergence resolved.** Nordic already unwinds and already suppresses the post-ACK NACK. ESP32
discards its START ACK result (`(void)od_cmd_reply(ctx, resp, sizeof(resp))`) and gains both.

### D3 — Fatal state: two predicates, not one

**Decision.** `OD_XFER_FATAL` gains meaning, and `od_xfer.h` adds exactly two lifecycle queries
(the existing owner and start-time queries remain):

```c
bool od_xfer_owns_hardware(void);      /* a live transfer holds panel/write resources   */
bool od_xfer_frames_may_arrive(void);  /* frames for this transfer are still expected   */
```

`OD_XFER_FATAL` answers **false** to the first and **true** to the second, until a replacement
START or a reset. Every other non-idle mode answers true to both; `OD_XFER_IDLE` answers false to
both.

`od_xfer_replace_active()`, `od_xfer_abort_active()` and `od_xfer_reset()` call
`od_xfer_app_abort()` only when `od_xfer_owns_hardware()` is true. Entering fatal releases hardware
exactly once; a later reset, disconnect or replacement clears state without issuing a second
target abort. START-ACK substitution clears the transfer rather than entering fatal. Every fatal
DATA path — explicit DATA NACK, consume failure and substituted SACK — enters fatal so in-flight
frames remain log-suppressed until reset or replacement.

**Why two.** A fatal PIPE transfer has released its hardware but still expects frames, because the
target suppresses per-frame logging until the client stops. One predicate cannot express both, and
ESP32 already carries the pair (`transferActive()` and `imageWriteFramesMayStillArrive()`).
At cutover those two ESP32 functions are **retained** but reduced to these shared queries.

### D4 — Negotiated frame bounds use the original on-wire length

**Decision.** `client_max_frame` is the largest complete on-wire DATA frame, matching the canonical
struct documentation and `py-opendisplay`. The dispatcher extends `od_cmd_ctx_t` with immutable
request metadata populated before decrypt:

```c
uint16_t wire_len;       /* original complete request length, before od_gate_open() */
bool     was_protected;  /* this request successfully crossed the CCM open path       */
```

The dispatcher remains the only production constructor. Direct-call host fixtures use one helper,
`tests/host/od_cmd_test_ctx.h`:

```c
static inline od_cmd_ctx_t od_test_cmd_ctx(od_reply_t rp,
                                           od_tx_reservation_t *r,
                                           uint16_t wire_len,
                                           bool was_protected)
{
    od_cmd_ctx_t ctx;
    ctx.rp = rp;
    ctx.r = r;
    ctx.wire_len = wire_len;
    ctx.was_protected = was_protected;
    return ctx;
}
```

All four arguments are mandatory; there is no defaulting wrapper. A fixture for a plaintext
command normally supplies `2 + body.n`, while a protected post-open fixture supplies the original
sealed length. A permanent `check.sh` ratchet rejects bare declarations, aggregate initializers and
file-local constructors of `od_cmd_ctx_t` under `tests/host/`. It keys on **non-pointer object
construction**, not the type name: `const od_cmd_ctx_t *ctx` parameters and other pointer
declarators are permitted, and `od_cmd_test_ctx.h` is the sole general construction exception.
Two exact storage-only exceptions are named for the fixtures that currently have a file-scope
`CTX`: `nordic_cmd_device_test.c` and `pipe_write_test.c` may each declare one non-const static
`od_cmd_ctx_t CTX`, while a paired check requires its assignment from `od_test_cmd_ctx()` inside
that file's existing `reset_all()`. The `pipe_write_test.c` exception is temporary: step 6b deletes
the frozen oracle and retires both its allowance and paired check in the same commit. The permanent
end state names only `nordic_cmd_device_test.c`. No third exception and no alternate initializer
are allowed. This covers both quiet C failure modes: omitted aggregate members becoming zero and
field-by-field locals leaving the new members indeterminate, without mistaking the 37 pointer
declarations for objects.

At START, compute the minimum frame capable of carrying one DATA byte before any subtraction:

- plaintext/TLS: `PIPE_FRAME_OVERHEAD + 1` = **4**;
- CCM-protected: that plaintext minimum plus `OD_SESSION_ENVELOPE_MIN` = **33**.

`OD_SESSION_ENVELOPE_MIN` is the 29-byte seal overhead: 16-byte nonce + one encrypted **inner
length byte** + 12-byte tag. That inner length byte is not PIPE's sequence byte, which is already
counted in `PIPE_FRAME_OVERHEAD`; do not add it twice and derive 34.

If `client_max_frame` is below the applicable minimum, NACK with
`OD_ERR_PIPE_START_BAD_HEADER`. Then, on every active, owned, non-empty DATA frame, enforce in this
order:

- `ctx->wire_len >= PIPE_FRAME_OVERHEAD`; anything smaller is unreachable through production
  dispatch and enters fatal `0x03`, making an omitted fixture field fail deterministically;
- `ctx->wire_len <= frame_eff` — the actual original frame, including a CCM envelope when present;
- `payload.n <= frame_eff - PIPE_FRAME_OVERHEAD - protection_overhead`, where
  `protection_overhead` is zero for plaintext/TLS and the session's 29-byte seal overhead for CCM.

The START floor and DATA `wire_len` floor are checked before their respective subtractions. D1's
inactive, fatal, empty and wrong-owner silent exits retain precedence, so the fixture tripwire does
not change their wire behaviour. `od_pipe.c` does not infer protection from global session state:
the dispatcher records what happened to this frame, so deferred/plain/TLS paths cannot be confused
with successfully opened CCM traffic.

**Why.** The current target code stores `frame_eff` but never enforces it. Checking `body.n + 2`
after dispatch decrypts only measures the plaintext frame; an encrypted peer could exceed its
negotiated on-wire maximum by the 29-byte envelope. A protected `frame_eff=4` is also unusable even
though four bytes can carry one plaintext DATA byte.

**Wire effect.** Refuses malformed or nonconforming clients only. Isolated commit and tests pin
plaintext 3/4 and protected 32/33 START floors, the production-unreachable DATA `wire_len` 2/3
boundary, plus negotiated maxima below the transport ceiling. The DATA floor needs no hardware row:
a production handler call necessarily already contains the two-byte command and one-byte sequence.

### D5 — Reorder geometry derives from the protocol, and the queue asserts move into shared code

**Decision.**

- Reorder payload width is `PIPE_MAX_FRAME - PIPE_FRAME_OVERHEAD` = **241**, replacing the
  deployed 248. D4 normally proves an admitted payload fits, but that is not the memory-safety
  backstop: immediately before any slot `memcpy`, shared PIPE independently checks
  `payload.n <= OD_PIPE_REORDER_PAYLOAD` and enters fatal `0x03` otherwise.
- Slot count is `OD_PIPE_MAX_W + 1`, replacing two hand-maintained `PIPE_REORDER_SLOTS` literals.
  `W + 1` is what keeps `seq % SLOTS` collision-free for a live window spanning at most `W` seqs;
  it is asserted, not asserted-by-comment.
- `OD_PIPE_MAX_W` and `OD_PIPE_MAX_N` become the target-set compile-time facts (defaults 32,
  `PIPE_SMALL_DRAM_WINDOW` ESP32 boards set 16). `PIPE_REORDER_SLOTS` and
  `PIPE_REORDER_SLOT_SIZE` are deleted from both targets.
- Because `shared/` can now see the window, the `OD_RXQ_SLOTS >= OD_PIPE_MAX_W + 2` and
  `OD_TXQ_SLOTS >= OD_PIPE_MAX_W + 2` assertions move into `shared/core/od_pipe.h`, and their
  target copies plus the "shared/ cannot see PIPE_MAX_W" comments are deleted. Under
  `OD_CAP_PIPE=0` these assertions are compiled out, not weakened.

**Effect.** 33×241 = 7,953 B (−231 B) on the wide-window targets; 17×241 = 4,097 B (−119 B) on
the classic-ESP32 boards. Recorded as a measurement, not claimed as a goal.

**D4/D5 invariant.** Negotiated admission and physical slot width are two belts. D4 limits the
actual wire frame; D5 separately bounds the copy by the destination object. Reverting or weakening
either check must not make the other disappear, and a direct production-machine test supplies
inconsistent command metadata to prove a 242-byte payload still reaches the width guard and is
refused before `memcpy`.

### D6 — Compression admission for PIPE: START shape decides, the config bit does not

**Decision.** Shared PIPE applies **no** `TRANSMISSION_MODE_STREAMING_DECOMPRESSION` gate.
`PIPE_FLAG_COMPRESSED` alone selects a compressed transfer, exactly as a four-byte legacy START
prefix alone selects one in `od_xfer_direct.c` today.

**Why.** This is the master plan's § 4.2 authority decision applied consistently: every current
build links a usable inflater, the advertised `transmission_modes` bits tell the host what to
*choose to send*, and a device that accepts compressed `0x70` while refusing compressed `0x80` for
the same image makes the host's choice look arbitrary. Resolves `docs/FOLLOWUPS.md` § 3.10 in the
"advisory" direction and closes the inconsistency Phase 2 opened.

**Obligation.** Update `docs/FOLLOWUPS.md` § 3.10 to record the resolution and its date in the
same commit that lands the admission rule. The corresponding `py-opendisplay` note stays external
follow-up work — conforming hosts are unaffected because they already gate on the advertised bits.

### D7 — Ownership is the full `{origin, tag}`, on both targets

**Decision.** PIPE ownership is `od_xfer_state()->owner`, the complete immutable `od_reply_t`
copied at START, checked through the existing `od_xfer_owner_matches()`.

**Divergence resolved.** ESP32 tracks a file-static `sessionOrigin` and compares **origin only**
(`frameOwnsSession()`), so two connections on the same transport are indistinguishable owners.
Nordic's target PIPE tracks no owner at all for DATA. Both are replaced by the § 4.1 rule the rest
of the transfer plane already follows. `sessionOrigin` is deleted with the ESP32 machine.

**Wire effect.** A DATA/END frame from a second BLE connection during a live PIPE transfer becomes
inert where ESP32 previously accepted it. Tested, and given a hardware row on ESP32.

### D8 — "No PIPE on LAN" is a shared origin rule, with no new seam and no new capability

**Decision.** Shared `od_pipe` refuses all three opcodes when `ctx->rp.origin != OD_ORIGIN_BLE`,
emitting the bytes ESP32 emits today:

- `0x80` → `[FF][80][OD_ERR_PIPE_START_BAD_HEADER][00]`;
- `0x81` / `0x82` → `[FF][81][00][00]` / `[FF][82][00][00]`.

All three use `od_xfer_reply_app()`, preserving ESP32's existing seal-or-plain choice: TLS-LAN and
security-off LAN are queued directly, while a successfully authenticated LAN-plain CCM session is
sealed. These are transport-policy refusals, not the plaintext protocol-error paths in § 5.3. A
reply substitution still returns `OD_CMD_NACK` and emits nothing further.

The refusal is **inert**: it touches no transfer state, aborts no session, tears down no panel,
and returns `OD_CMD_NACK`. On Nordic and BG22 no non-BLE origin exists, so the rule never fires
and costs one comparison.

**Why no capability macro.** The rule is unconditional protocol policy (canonical header § 9
rule 2) and this repository is explicitly not making PIPE legal on LAN. A macro would invite a
target to switch it off; a seam would re-import wire policy into target code. Both are worse than
a comparison that is provably inert on two of three targets.

`pipe_refused_on_lan()` and its ESP32 log line are deleted with the target machine. The log line's
information — which opcode, from which origin — moves into the shared refusal through `od_log`.
The header freeze means `OD_ERR_PIPE_START_BAD_HEADER` continues to be reused for "wrong
transport"; a dedicated code stays external follow-up work.

### D9 — BG22 capability-off shape, byte-exact

**Decision.** With `OD_CAP_PIPE=0` the shared entry points are compiled to:

- `od_pipe_start()` → `od_xfer_reply_error()` with `{FF, 80, 04, 00}`, return `OD_CMD_NACK`;
- `od_pipe_data()` → emit nothing, return **`OD_CMD_UNKNOWN`**;
- `od_pipe_end()` → emit nothing, return **`OD_CMD_UNKNOWN`**.

These are the bytes and the verdicts `od_cmd_silabs.c` produces today and they are preserved
exactly — note that DATA/END return `OD_CMD_UNKNOWN`, **not** D1's `OD_CMD_NACK`, because on a
target without the capability the opcode genuinely is unknown, and `od_frame_policy()` gives an
unknown opcode no activity stamp. `0x04` remains target-specific; this repository assigns it no
canonical meaning.

No reorder array, no `od_pipe_state_t`, no sequence counters and no PIPE-mode branch of the shared
transfer machine may exist in the BG22 image. The three small shared capability-off entry points
necessarily remain because dispatch references them. Map/symbol inspection proves that only those
entry points remain and that they pull in no PIPE state or reorder storage.

The existing dispatch budget expression `(OD_CAP_PIPE ? 3u : 1u)` for `0x81`/`0x82` is retained
verbatim.

### D10 — PIPE auto-completion is protocol, not the target's legacy knob

**Decision.** Uncompressed **full-frame** PIPE auto-completes when `written_bytes` reaches
`expected_bytes`, on every PIPE-capable target, independently of `OD_XFER_DIRECT_AUTO_END`.
Compressed PIPE and partial PIPE never auto-complete; they require the explicit `0x82`.

**Why this is called out.** `OD_XFER_DIRECT_AUTO_END` is 1 on ESP32 and 0 on Nordic, and it
governs the *legacy* `0x71` path only. Both targets' PIPE machines auto-complete today regardless.
Wiring PIPE's auto-END to that macro would silently disable it on Nordic — a wire regression
invisible in review. The shared PIPE auto-END path is unguarded by that macro and a test asserts
so under both macro values.

The auto-END reply sequence is fixed: tail SACK first, then the `{00,82}` END ACK, then barrier,
refresh, `{00,73}`/`{00,74}` — a maximum of three replies from one DATA frame, which is exactly
the reservation budget of 3.

### D11 — Cadence and SACK policy are frozen; the tail flush is not fixed inside this phase

**Decision.** The gap/cadence policy moves unchanged:

- in-order accept increments `frames_since_ack` for the trigger frame and every drained
  successor, and SACKs when it reaches `n_eff`;
- the first ahead-of-window arrival opens the gap and SACKs immediately (fast retransmit), then
  further out-of-order or duplicate arrivals SACK once per `n_eff`;
- a duplicate below `expected_seq` but within `W` is discarded and SACKed under the same
  rate limit;
- outside the window in both directions is fatal (`0x04`);
- reorder-queue occupancy reaching the slot count is fatal (`0x03`);
- `payload.n > OD_PIPE_REORDER_PAYLOAD` is fatal (`0x03`) before slot selection or `memcpy`,
  independent of D4's negotiated-frame checks;
- a consume failure is fatal with `0x02` when compressed and `0x03` when not.

If § 2.4 reproduces the small-tail stall, the fix lands as a **separate commit after** the
promotion completes, in shared code, with its own tests and its own hardware row. It does not ride
inside the move. A move commit that also changes flush timing cannot be bisected against either
failure.

### D12 — Etag endianness, pinned in both directions

**Decision.** `old_etag` in the 12-byte partial START extension is **little-endian** (it is part
of the LE PIPE header family); `new_etag` on the `0x82` END tail is **big-endian** at
`payload[1..4]`, present only when the END body is at least 5 bytes. Both targets already agree;
neither states it. A test pins each, with a byte-swapped negative case.

### D13 — Reply-failure verdicts on the completion tail

**Decision.** The END/auto-END tail reports what actually went out:

- END ACK substituted → abort, no refresh, no further reply, `OD_CMD_NACK`;
- barrier abort → `od_xfer_app_barrier_abort()` exactly once, no refresh, no further reply,
  `OD_CMD_NACK`;
- `od_xfer_app_refresh()` returning false → abort with `OD_XFER_ABORT_REFRESH_FAILED`, clear a
  partial/present etag, emit the plaintext `{FF,82}` after the already-queued END ACK, and return
  `OD_CMD_NACK`; this preserves Nordic's deployed tail and the shared legacy failure policy;
- refresh timeout with the `{00,74}` reply queued successfully → `OD_CMD_OK`; the transfer
  completed and the panel was driven, so the frame was accepted;
- refresh status reply itself substituted → `OD_CMD_NACK`, because the only thing the host
  received for that frame was a hard NACK.

This is Nordic's current tail behaviour, adopted for both targets.

### D14 — Reservation budgets unchanged

`1` for `0x80`, `3` for `0x81`, `3` for `0x82` under `OD_CAP_PIPE`, `1` each when off. Pinned in
dispatch tests **before** the reroute and again after. A budget change is a separate wire-policy
decision and is not part of Phase 3.

---

## 5. Architecture

### 5.1 New files

```text
shared/core/od_pipe.h       public: three command entry points, the two sizing facts, the queue asserts
shared/core/od_pipe.c       negotiation, sequence/reorder, SACK, cadence, completion (APP_XFER tier)
```

`od_pipe.c` joins `OD_SHARED_SOURCES_APP_XFER` in `shared/sources.cmake` — listed once, never
globbed. Every `APP_SESSION` consumer already takes `APP_XFER`, so **all three targets link it
from the first commit**, which is why D9's capability-off arm must exist in that same commit
rather than arriving with the BG22 cutover (§ 6, staging correction C2).

### 5.2 Internal reply-free operations — header-first

`od_xfer_data()` and `od_xfer_end()` cannot be reused: they route on the legacy modes and emit
`0x71`/`0x72` replies. But PIPE is not a new public surface either. The PIPE peers of the existing
`od_xfer_direct_data_impl()` / `od_xfer_partial_end_impl()` are added **to
`shared/core/od_xfer_internal.h`, beside them**, and to nothing else:

```c
/* All reply-free. od_pipe.c owns every PIPE wire byte; od_xfer owns state, accounting, hardware. */
bool od_xfer_pipe_arm_full(const od_cmd_ctx_t *ctx, uint32_t total, bool compressed);
#if OD_CAP_PARTIAL
bool od_xfer_pipe_arm_partial(const od_cmd_ctx_t *ctx, uint32_t total, bool compressed,
                              uint32_t old_etag, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              uint8_t *err_out);
#endif
bool od_xfer_pipe_activate(void);              /* begin_full / begin_partial, post-ACK           */
bool od_xfer_pipe_consume(od_span_t payload);  /* ordered bytes -> the one sink and one counter  */
bool od_xfer_pipe_finalize(void);              /* flush a compressed stream                      */
bool od_xfer_pipe_complete(void);              /* byte-accounting completeness test              */
void od_xfer_pipe_enter_fatal(void);           /* mode -> OD_XFER_FATAL, hardware released       */
od_xfer_barrier_t od_xfer_pipe_before_refresh(void); /* flush/drain barrier, owner from state       */
void od_xfer_pipe_barrier_abort(void);          /* barrier recovery exactly once + clear state     */
bool od_xfer_pipe_refresh(uint8_t mode, bool has_new_etag, uint32_t new_etag,
                          bool *completed);     /* refresh + etag policy + clear state              */
void od_pipe_reset_state(void);                /* sequencing/reorder only; called by od_xfer     */
```

Writing this header **before** `od_pipe.c` is what prevents a second byte counter, a second
inflater driver or a second END policy appearing despite the stated design. `od_pipe.c` contains
no call to `od_xfer_app_*` and no `od_zlib_pump_*` call at all.

The completion split is deliberate: `od_pipe.c` first queues the END ACK, then asks
`od_xfer_pipe_before_refresh()` to run the barrier, then calls `od_xfer_pipe_refresh()`. The last
operation calls the target refresh, applies full/partial etag policy and clears transfer state;
false means the target refresh operation itself failed and D13's plaintext `{FF,82}` is still the
PIPE machine's responsibility. `od_xfer_pipe_barrier_abort()` is the sole path to the target's
barrier-abort seam. No internal operation constructs or queues a reply.

Reset is one entry point: every route that reaches `od_xfer_clear_state()` also calls
`od_pipe_reset_state()` exactly once; `od_xfer_reset()` reaches it through that path rather than
calling it a second time. The reset is compiled to nothing under `OD_CAP_PIPE=0`. Targets never
call a PIPE reset. Fatal-state reset/replacement follows D3 and does not abort already-released
hardware again.

### 5.3 Replies

`od_pipe.c` builds bytes and hands them to the existing single choice sites — no new sealing
logic, no `od_reply()` call of its own:

| Reply | Path | Function |
|---|---|---|
| START ACK (8 B) | sealed | `od_xfer_reply_app()` |
| SACK — cadence, gap, duplicate, tail (7 B) | sealed | `od_xfer_reply_app()` |
| END ACK `{00,82}` | sealed | `od_xfer_reply_app()` |
| Refresh `{00,73}` / `{00,74}` | sealed | `od_xfer_reply_app()` |
| START NACK `[FF][80][err][00]` | plaintext | `od_xfer_reply_error()` |
| DATA NACK `[FF][81][err][hs][mask:4]` | plaintext | `od_xfer_reply_error()` |
| END NACK `{FF,82}` | plaintext | `od_xfer_reply_error()` |
| LAN refusal (D8, 4 B) | seal-or-plain | `od_xfer_reply_app()` |

Every one of these is asserted under both a live encrypted session and a plaintext one; payload
bytes alone cannot distinguish the two paths.

### 5.4 Compile-time surface

Added compile-time facts: `OD_PIPE_MAX_W`, `OD_PIPE_MAX_N` (D5). Retained: `OD_CAP_PIPE`,
`OD_CAP_PARTIAL`. D4's `od_cmd_ctx_t` metadata is a runtime struct extension described in D4, not
part of this compile-time surface.
Deleted at cutover: `PIPE_MAX_W`, `PIPE_MAX_N`, `PIPE_REORDER_SLOTS`, `PIPE_REORDER_SLOT_SIZE`
from both targets; `PIPE_SMALL_DRAM_WINDOW` survives as the ESP32 board switch that sets
`OD_PIPE_MAX_W`/`OD_PIPE_MAX_N` to 16.

Static assertions: `1 <= OD_PIPE_MAX_N <= OD_PIPE_MAX_W <= 32`; `OD_PIPE_MAX_W <=
PIPE_ACK_MASK_BITS`; slot count `== OD_PIPE_MAX_W + 1`; slot payload `== PIPE_MAX_FRAME -
PIPE_FRAME_OVERHEAD`; `OD_RXQ_SLOTS`/`OD_TXQ_SLOTS >= OD_PIPE_MAX_W + 2`; wire struct sizes for
`PipeStartRequest` and `PipePartialExt`; `OD_SESSION_ENVELOPE_MIN == 29u`; and the protected
one-byte DATA minimum is 33.

---

## 6. Staging

Steps run in order. **Steps 1-5 reroute no production opcode.** Three corrections to the master
plan's staging are marked C1-C3 and stated with their reason.

### Step 1 — Freeze the reference behaviour before deleting it

1. Diff `../Firmware/src/display_service.cpp` and `../Firmware_NRF54`'s PIPE source against the
   unified snapshots. Record deliberate unified adaptations (`od_reply`/`od_txq` verdicts,
   `od_xfer` arbitration, logging) separately from donor drift. Sibling repositories stay
   read-only; anything found wrong upstream is filed, not fixed.
2. Extend `tests/host/pipe_write_test.c` — which compiles Nordic's **production** machine — to
   cover every § 4 behaviour it does not already, so the reference is executable at the moment of
   deletion. This suite becomes the differential oracle for the shared machine and is retired only
   in step 6b.
3. Capture the current `.text`/`.rodata`/`.data`/`.bss` per target, BG22 heap-inclusive size, and
   PIPE throughput/refresh timings on any available board.

### Step 2 — Land the internal operations header and its fake-target tests

`od_xfer_internal.h`'s § 5.2 additions, implemented in `od_xfer.c` / `od_xfer_direct.c` /
`od_xfer_partial.c` and exercised **directly** against the existing fake `od_xfer_app`. No
`od_pipe.c` yet. Pin: arming under both PIPE modes; owner capture; offsets monotonic and
non-empty into the sink; short consumption refusing; finalize and completeness; fatal entry
releasing hardware exactly once; fatal followed by reset, disconnect-equivalent reset and
replacement causing no second abort; barrier proceed/abort; refresh success, timeout and target
failure; full and partial etag policy. The `od_pipe_reset_state()` call is wired when `od_pipe.c`
lands in step 4, so step 2 does not introduce an unresolved reverse dependency.

### Step 3 — Land D4's command-context metadata as its own commit

Extend `od_cmd_ctx_t` with `wire_len` and `was_protected`, populate both at the two dispatcher
construction sites, add `tests/host/od_cmd_test_ctx.h`, and replace every direct host-test
construction with `od_test_cmd_ctx()` carrying all four explicit values. Delete file-local
`make_ctx()`/`reserve_ctx()` variants rather than teaching each about the fields independently.
Two files cannot substitute the inline helper into their current static-constant initializer:
demote `tests/host/nordic_cmd_device_test.c`'s and `tests/host/pipe_write_test.c`'s file-scope
`CTX` to a non-const runtime-initialized file-scope object and assign it from
`od_test_cmd_ctx()` in each existing `reset_all()`. The former has 15 use sites; the frozen
Nordic oracle has 11 and is deleted in step 6b, so do not churn those 26 call sites into per-test
locals. Both files document that the metadata is not consumed by the handlers under test. Mark
the oracle's storage allowance and paired assignment check as temporary through step 6b; the
device-test allowance is the only permanent exception.

Install the permanent `check.sh` construction ratchet in this commit. Match object constructions,
not every `od_cmd_ctx_t` token: explicitly exclude pointer declarators in parameter lists and local
declarations, and exclude `od_cmd_test_ctx.h`. Reject an uninitialized automatic object, an
aggregate initializer or an alternate context-returning helper everywhere else under
`tests/host/`. Allow exactly the two named static storage declarations above, with companion checks
requiring the `reset_all()` assignment through `od_test_cmd_ctx()`; an initializer from that helper
is the normal allowed form. Add dispatch/context tests that pin the original pre-decrypt length and
successful-CCM marker, including plaintext, TLS, successful CCM and rejected-gate paths. No
`od_pipe.c`, sizing change or production opcode reroute lands here. This is a shared-struct ABI
sweep and stays isolated so review can distinguish the mechanical initializer changes from PIPE
policy.

### Step 4 — Land `od_pipe.c` dormant, with the capability-off arm in the same commit **[C2]**

**Correction C2.** The master plan puts BG22's capability-off arm in its dormant-machine step and
implies it can arrive with a later cutover. It cannot: `od_pipe.c` enters the `APP_XFER` tier that
BG22 already links, so from this commit BG22 compiles and links it. D9's arm therefore lands here,
with the BG22 map/symbol test, in the same commit. BG22's target hooks stay in place and win at
link time until step 8 — a dormant shared function and a live target hook cannot both define
`od_cmd_app_pipe_start`, so the shared entry points are named `od_pipe_*` from the first line and
never `od_cmd_app_*`.

Build and test the three configurations: `OD_PIPE_MAX_W=32`, `OD_PIPE_MAX_W=16`, and
`OD_CAP_PIPE=0`. This commit consumes step 3's metadata to implement D4's protection-aware bounds
including the production-unreachable `wire_len < PIPE_FRAME_OVERHEAD` fatal `0x03` tripwire, and
lands D5's derived reorder geometry/assertions; both remain dormant because dispatch still names
target hooks. Create no PIPE decompressor and no second byte total.

### Step 5 — Full shared-machine test suite, still dormant

The complete § 7 suite runs against `od_pipe_*` called directly, before any production frame
reaches it, including the differential comparison against step 1's frozen Nordic oracle.

### Step 6 — Per-target cutover, hardware-gated between targets

Repository target order: **ESP32, then Nordic.** BG22 has no target PIPE machine to delete and is
handled entirely by step 8.

**6a — ESP32.** In one commit: delete the master plan § "ESP32 inventory, frozen by the landed 10a
cutover" delete-list; retain its retain-list verbatim; reduce `transferActive()` and
`imageWriteFramesMayStillArrive()` to D3's shared predicates; delete `sessionOrigin` and
`frameOwnsSession()`; delete `pipe_refused_on_lan()`; delete the two
`od_xfer_app_prepare_start()` calls that cancelled target PIPE, then delete
`cleanupDirectWriteState()` and `cleanup_partial_write_state()` **only** once each has no
remaining caller; delete `PIPE_REORDER_SLOTS`/`PIPE_REORDER_SLOT_SIZE`/`PIPE_MAX_W`/`PIPE_MAX_N`
and the duplicated queue assertions from `structs.h`, setting `OD_PIPE_MAX_W`/`OD_PIPE_MAX_N`
instead; delete now-unused declarations from `display_service.h`, `main.h` and `structs.h`.
Reduce the three existing `od_cmd_app_pipe_*` definitions in `od_cmd_app.cpp` to temporary,
one-line forwarding wrappers around `od_pipe_*`; dispatch still names the hook seam until step 8,
so removing these definitions here would be a link failure. The wrappers contain no policy and
are deleted in step 8.
In the same commit retire `esp32_xfer_interim_pipe_arbitration()` and
`esp32_xfer_10b_inventory()` from `tools/check.sh` and install the ESP32-scoped single-pump-owner
ratchet (§ 8). **Then pass the ESP32 hardware gate (§ 9) before touching Nordic.**

**6b — Nordic.** The same shape against the master plan's Nordic inventory: delete
`opendisplay_pipe_write.{cpp,h}` entirely along with the display-state block retained only for
target PIPE; retain the `XferApp*` adapter primitives verbatim; remove the
`od_xfer_app_prepare_start()` PIPE-cancel call and then delete `partial_cleanup()` and
`opendisplay_display_abort()` only once no non-PIPE caller remains; retire
`nordic_xfer_interim_pipe_arbitration()` and `nordic_xfer_10b_inventory()`; install the
Nordic-scoped pump ratchet. Move only the three `od_cmd_app_pipe_*` definitions into
`od_cmd_device.c` as temporary one-line forwarding wrappers around `od_pipe_*`, for the same
link-time reason as 6a. Remove `od_pipe_write_test` and the `opendisplay_pipe_write.cpp` entries
from `tests/host/CMakeLists.txt` — including the `od_dispatch_corpus_nordic_test` source list — in
the same commit, after step 5 proved the shared suite covers every case the frozen oracle did.
Delete `tests/host/pipe_write_test.c` itself, and retire its named context-storage allowance and
paired `reset_all()` assignment check in this commit; neither a positive check against the absent
file nor a conditional inert allowance survives. The Nordic corpus already compiles
`od_cmd_device.c`, so it exercises the shared machine through the temporary production hooks.
**Then pass the Nordic hardware gate.**

**Leaving an interim ratchet to fail on an absent function is not an acceptable implementation.**
Each retirement is part of the commit that makes it stale.

**Correction C1 — the sequencing-exception protocol.** If project direction lands 6b before the
ESP32 gate closes (as happened twice in Phase 2), then: both targets' rows stay open, neither
target is described as qualified anywhere, and the exception is written into
`docs/HARDWARE_VERIFICATION_CHECKLIST.md` at the time it is taken. The exception buys implementation
order; it never buys evidence.

### Step 7 — Finish reset ownership

With PIPE inside the shared singleton, `od_core_reset()` calls `od_xfer_reset()` **first**, ahead
of `od_config_read_cancel()` / `od_txq_reset()` / `od_session_clear()` — producer, then egress,
then session, the documented ordering. `od_core.c` is already in a tier whose consumers all link
`od_xfer`, so no `sources.cmake` change is needed.

Targets then stop calling `od_xfer_reset()` and `opendisplay_pipe_write_reset()` from their
disconnect paths: ESP32's `session_guard.cpp` and Nordic's `opendisplay_pipe.c` teardown lose
their explicit calls. The `check.sh` orderings that currently require `od_xfer_reset` **before**
`od_core_reset` in those two files are replaced by one shared assertion that `od_core_reset()`
resets the transfer before the egress queue.

**Correction C3.** The reset-ownership step follows both target cutovers, which would leave each
target's disconnect path calling a target PIPE reset that no longer exists between 6a and 6b.
Step 7 is therefore split: the per-target half of the disconnect rewiring lands **inside that
target's own step-6 commit**, and step 7 proper only moves `od_xfer_reset()` into
`od_core_reset()` once **both** PIPE-capable targets have cut over.

### Step 8 — Reroute dispatch, delete every hook, install the permanent ratchet

1. `OD_DISPATCH_OPCODE_ROWS` names `od_pipe_start`, `od_pipe_data`, `od_pipe_end`; budgets
   `1`/`3`/`(OD_CAP_PIPE ? 3u : 1u)` unchanged (D14).
2. The three declarations leave `od_cmd_app.h`; ESP32's and Nordic's temporary forwarding
   definitions and BG22's three definitions in `od_cmd_silabs.c` are deleted here — their bytes
   now come from shared `od_pipe`.
3. `check.sh` rejects reintroduction of `od_cmd_app_pipe_start`/`_data`/`_end` by symbol.
4. Collapse the two target-scoped pump ratchets into one repository-wide production invariant:
   only `shared/core/od_xfer.c` calls `od_zlib_pump_reset()` or `od_zlib_pump_push()`. Scope it to
   `targets/` plus `shared/core/` **excluding** `od_zlib_pump.c`'s own definitions, because
   `tests/host/zlib_pump_test.c` legitimately drives the pump directly and must keep doing so.

### Step 9 — Evidence

Re-measure § 10, update `docs/HARDWARE_VERIFICATION_CHECKLIST.md` and the CLAUDE.md status
section, and record which rows remain open on which unavailable board.

---

## 7. Tests

All host tests call the **production** shared machine. No fake ever sees an expected reply.

**Negotiation and START.** Body lengths 0..9, 10, 21, 22, 23 and trailing-byte tolerance; version
≠ 1; every undefined flag bit; `flags` bit1 without the 12-byte extension; W and N of 0, 1, 16, 17,
32, 33, 255 through the min/floor/`N<=W` rules; plaintext `client_max_frame` of 0, 1, 2, 3, 4,
243, 244, 245 and 65535 with the acceptance boundary at 4; protected `client_max_frame` of 31, 32,
33, 243, 244 and 245 with the acceptance boundary at 33 (D4); total-size mismatch; every
`OD_ERR_PIPE_START_*` code emitted from the branch that owns it and from no other; partial rect
validation, etag match/mismatch, 8-pixel alignment and overflow arithmetic; `od_color`-unsupported
schemes refused with no panel activation; compressed START accepted with no config gate (D6).
Every malformed, unsupported and size-mismatched BLE START is repeated while each transfer mode is
live and must displace it before returning its NACK; the same LAN stimuli remain inert (D2/D8).

**START ordering (D2).** ACK queued before activation; substituted ACK unwinds armed state, clears
a partial etag and emits nothing; post-ACK activation failure leaves `OD_XFER_FATAL` with no START
NACK, and the following DATA frame is silent. Fatal followed by reset or replacement does not call
the target abort a second time.

**Sequence and reorder.** In-order run; every arrival permutation for W=4 and a sampled set for
W=16/32; gap open and close; drain of contiguous successors incrementing cadence per frame;
duplicates inside the window; sequence wrap across 255→0 including a wrap inside the reorder
queue; mask bits 0 and 31; `highest_seen` advance rule at exactly `PIPE_ACK_MASK_BITS`; occupancy
reaching the slot count as fatal `0x03`; out-of-window both directions as fatal `0x04`; explicit
241/242-byte reorder-copy boundary with deliberately inconsistent direct-call metadata proving the
D5 width guard survives independently of D4.

**Frame bounds (D4).** `ctx->wire_len <= frame_eff` and the protection-aware payload bound on every
DATA frame, at the boundary and one either side. Plaintext runs use `frame_eff` 4, 100 and 244;
protected runs use 33, 100 and 244 and prove that a decrypted body which fits while its original
CCM envelope exceeds `frame_eff` is refused. With an active owned transfer and a non-empty direct
DATA call, `wire_len` 0, 1 and 2 each produce fatal `0x03`, while 3 reaches ordinary DATA handling;
the inactive/fatal/empty/wrong-owner precedence cases remain as specified by D1. The permanent
construction check is mutation-validated against both forbidden forms — a zero-filled aggregate
and a bare field-by-field local — while accepting an initializer from `od_test_cmd_ctx()`. Its
false-positive control includes representative `const od_cmd_ctx_t *ctx` parameter and local
pointer declarations. Separate mutations remove each named fixture's `reset_all()` assignment or
introduce a third static storage exception; both must fail.

**Verdicts (D1).** Inactive/fatal/zero-length/wrong-owner DATA and active wrong-owner END →
`OD_CMD_NACK` with **zero** bytes emitted; inactive END from any owner and fatal-state owner END →
`OD_CMD_NACK` with the plaintext `{FF,82}`; genuinely absorbed duplicate → `OD_CMD_OK` with a SACK.
Assert verdicts, precedence and emitted-frame counts/bytes. Separately pin that
`OD_FRAME_HANDLER_NACK` and `OD_FRAME_ACCEPTED` have the same current activity/reset/consume policy,
so the ESP32 verdict correction does not claim a liveness change.

**Completion.** Uncompressed full auto-END under both values of `OD_XFER_DIRECT_AUTO_END` (D10);
compressed and partial never auto-completing; explicit END order tail SACK → END ACK → barrier →
refresh → `0x73`/`0x74`; auto-END capped at three replies; incomplete END refusing with
`{FF,82}` and clearing a partial etag; compressed exactness — DONE early, output overrun,
truncated input, checksum failure; etag endianness both directions (D12); target refresh returning
false after the END ACK emitting plaintext `{FF,82}`, aborting once and clearing the etag (D13).

**Reply protection.** Every row of § 5.3 asserted under a live session and a plaintext one.
Substitution injected at START ACK, cadence SACK, gap SACK, tail SACK, END ACK and refresh status,
each with its D13 verdict, with no reply following a substituted fatal NACK and no panel mutation
after any failure.

**Capability and origin.** `OD_CAP_PIPE=0`: exact bytes for all three opcodes, `OD_CMD_UNKNOWN`
for DATA/END (D9), only the three shared capability-off PIPE entry points in the BG22 map, and zero
PIPE state/reorder symbols or bytes. LAN origin (D8): exact four-byte refusal payloads on all three
opcodes, seal-or-plain behaviour under security-off LAN, authenticated LAN-plain and TLS-LAN,
complete inertness against a live BLE-owned transfer, and proof the rule never fires for
`OD_ORIGIN_BLE`.

**Integration.** `od_core_reset()` clearing PIPE sequencing, transfer state and hardware exactly
once, in the documented order; disconnect during each PIPE mode and from `OD_XFER_FATAL`;
replacement START of every mode by every other mode, including PIPE-over-PIPE and fatal-to-fresh,
with the displaced owner's frames inert and no second abort of fatal hardware.

**Model and differential.** A simple reference receiver plus model-based traces with loss,
duplication, reorder and wrap, compared against the shared machine. Step 1's frozen Nordic oracle
compared case-for-case before it is retired. Python sender tests stay the independent peer.

**Corpus.** PIPE vectors move from the Nordic profile's target machine to the shared machine; the
Silabs profile gains `target-production` vectors for D9's three capability-off answers. A
`target-production` vector excluded by a capability predicate is a **failure**, per C12.

**Mutation checks.** The suite must fail when a mutant: drops the owner check, mis-sizes the byte
total, skips a reset, drops a reply status, corrupts one SACK mask bit, removes D4 while offering a
242-byte reorder payload, removes D4's minimum `wire_len` tripwire, removes D5's independent
copy-width guard, bypasses the single fixture constructor, or allocates reorder storage in a
capability-off build.

**Fuzz.** START and DATA parsing added to the existing pre-auth fuzz targets.

---

## 8. Ratchet transitions in `tools/check.sh`

| Check | Fate |
|---|---|
| `esp32: interim PIPE/od_xfer arbitration` | deleted in step 6a |
| `esp32: Phase 2 step 10b inventory` | deleted in step 6a |
| `nordic: interim PIPE/od_xfer arbitration` | deleted in step 6b |
| `nordic: Phase 2 step 10b inventory` | deleted in step 6b |
| `esp32: shared legacy transfer cutover` | `od_xfer_reset` clause moves to the shared check in step 7 |
| `nordic: shared legacy transfer cutover` | same |
| *(new)* `command context: explicit host fixtures` | step 3 installs two checked storage exceptions; step 6b deletes `pipe_write_test.c` and its allowance/check; permanently, object construction under `tests/host/` goes through `od_test_cmd_ctx()`, pointer declarators are excluded, and only `nordic_cmd_device_test.c` retains a checked static slot |
| *(new)* `esp32: single pump owner` | added in 6a, folded into the repo-wide check in step 8 |
| *(new)* `nordic: single pump owner` | added in 6b, folded into the repo-wide check in step 8 |
| `silabs: shared legacy transfer cutover` | already carries BG22's permanent pump-owner clause; that clause folds into the repo-wide check in step 8 |
| *(new)* `transfer: single pump owner` | step 8, repository-wide, tests excepted |
| *(new)* `pipe: no target PIPE machine` | step 8, by symbol: no `od_cmd_app_pipe_*`, no `PipeWriteState`, `PipeReorderSlot`, `pipe_build_ack_payload`, `send_pipe_*`, `handlePipeWrite*`, `pipe_refused_on_lan`, `sessionOrigin` anywhere under `targets/` |
| *(new)* `pipe: dispatch routes shared` | step 8, rows name `od_pipe_*`, budgets `1`/`3`/`3` |

The transitional checks carry comments naming "Phase 3 step 6", written against the master plan's
old numbering. Whichever commit deletes each check deletes its comment with it; no comment is left
pointing at a step that no longer exists.

---

## 9. Hardware gates

Add to `docs/HARDWARE_VERIFICATION_CHECKLIST.md` a `## Transfer Phase 3 — PIPE` section with a
per-target block. Every row is new evidence; no Phase 1 or Phase 2 result qualifies it.

**ESP32 (`s3-n16r8-extuart-debug`, plus one classic-ESP32 `OD_PIPE_MAX_W=16` board if available):**

- [ ] plaintext and encrypted full-frame PIPE, raw and compressed, through refresh
- [ ] PIPE-partial: region streamed, partial refresh, new etag committed; etag mismatch refused
- [ ] forced loss, reorder and retransmission; gap SACK observed and the transfer recovering
- [ ] negotiated on-wire maximum enforced in plaintext and encrypted sessions: boundary frame
      accepted, one-byte-over frame refused, including a CCM case whose decrypted body alone fits
- [ ] tail below cadence completing without a stall (§ 2.4's row, re-checked post-promotion)
- [ ] sequence wrap past 255 inside one transfer
- [ ] END ACK observed on air before refresh begins; refresh success and timeout both observed
- [ ] `OD-S1` replay injection via `dispatch-gate`, including the corrupted-tag control
- [ ] inactive/fatal/zero-length DATA stays silent; inactive/fatal END answers plaintext `FF 82`
      (activity stamping remains the existing shared policy and is pinned in host tests)
- [ ] second-connection DATA during a live PIPE is inert (D7's tightening)
- [ ] LAN and TLS-LAN `0x80`/`0x81`/`0x82` refused with the exact four-byte payloads and the
      deployed seal-or-plain choice, leaving a live BLE-owned PIPE transfer untouched (D8)
- [ ] replacement in every direction: PIPE↔PIPE, PIPE↔legacy, and malformed BLE PIPE START
      displacing the live owner; displaced owner inert, fresh transfer succeeding
- [ ] disconnect mid-PIPE, reconnect, re-authenticate, complete a fresh upload
- [ ] `OD_PIPE_MAX_W=16` board: negotiated window of 16 honoured, reorder queue sized 17

**Nordic (`xiao_nrf52840` mandatory; one nRF54-class board):**

- [ ] plaintext and encrypted full-frame PIPE, raw and compressed, through refresh
- [ ] PIPE-partial: region streamed, partial refresh, new etag committed; etag mismatch refused
      (specifically repeated here because § 2.3 is its first hardware precedent)
- [ ] forced loss, reorder and retransmission; gap SACK observed and the transfer recovering
- [ ] negotiated on-wire maximum enforced in plaintext and encrypted sessions: boundary frame
      accepted, one-byte-over frame refused, including a CCM case whose decrypted body alone fits
- [ ] tail below cadence completing without a stall (§ 2.4's row, re-checked post-promotion)
- [ ] sequence wrap past 255 inside one transfer
- [ ] END ACK observed on air before refresh begins; refresh success and timeout both observed
- [ ] `OD-S1` replay injection via `dispatch-gate`, including the corrupted-tag control
- [ ] inactive/fatal/zero-length DATA stays silent; inactive/fatal END answers plaintext `FF 82`
      (activity stamping remains the existing shared policy and is pinned in host tests)
- [ ] replacement in every direction: PIPE↔PIPE, PIPE↔legacy, and malformed BLE PIPE START
      displacing the live owner; displaced owner inert, fresh transfer succeeding
- [ ] disconnect mid-PIPE, reconnect, re-authenticate, complete a fresh upload
- [ ] compressed PIPE on a device whose config lacks the streaming-decompression bit — accepted
      (D6's resolution, wire-visible)
- [ ] one nRF54-class board repeats full raw/compressed PIPE, forced reorder/recovery and the
      negotiated-frame bound; LAN and simultaneous-second-connection rows are omitted because
      Nordic exposes neither surface

**BG22 (`efr32bg22-slc`):**

- [ ] `0x80` answers `FF 80 04 00`; `0x81` and `0x82` answer nothing
- [ ] map confirms zero PIPE state, zero reorder storage, and static RAM at or below the
      recaptured Phase 0 baseline

ESP32 evidence never qualifies Nordic and Nordic never qualifies ESP32. An unavailable board
leaves its row explicitly open; it does not weaken an invariant and is not reported as a pass.

---

## 10. Software gate and acceptance

`ASAN_OPTIONS=detect_leaks=0 ./tools/check.sh --targets` at every step, reading the summary — a
skip is not a pass. Required coverage is the master plan § 8 list: gcc/clang/ASan/UBSan host
suites, all 11 ESP32 configurations plus the sdkconfig baseline, all three Nordic boards, the BG22
production-source tests and real ARM link, all three corpus profiles, the Python wire corpus,
capability permutations, map/symbol checks, the new fuzz targets and the mutation checks.

Record after each cutover: shared LOC added, target LOC removed, `.text`/`.rodata`/`.data`/`.bss`
per target, BG22 heap-inclusive size, reorder `.bss` before and after D5, stack high-water where
available, and PIPE throughput/retransmissions/refresh time.

Minimums at Phase 3 exit: one PIPE machine; zero target PIPE state structs; zero disabled-PIPE
large objects; no BG22 static-RAM regression; no unexplained throughput regression above 5%; net
handwritten production-source deletion (test growth reported separately). No new heap use, no
variable-length stack object, no full image buffer.

---

## 11. Stop conditions

Beyond the master plan's § 10 list, stop this phase when:

- `od_pipe.c` acquires a `od_zlib_pump_*` call, a second byte counter, or a direct
  `od_xfer_app_*` call;
- a PIPE reply is constructed anywhere except `od_pipe.c`, or sealed anywhere except the
  `od_xfer_reply_*` helpers;
- the capability-off arm allocates anything, or its bytes drift from D9;
- the LAN refusal acquires a capability macro, a target seam, or any effect on a live transfer;
- negotiated frame enforcement uses decrypted `body.n` as a substitute for D4's original
  `ctx->wire_len`, or accepts a protected maximum below 33;
- an active, owned, non-empty DATA path can reach frame-bound arithmetic with
  `ctx->wire_len < PIPE_FRAME_OVERHEAD`, or a host fixture constructs `od_cmd_ctx_t` without the
  single explicit helper, a currently named storage slot escapes its checked `reset_all()`
  assignment, or `pipe_write_test.c`'s allowance/check survives step 6b;
- PIPE auto-END becomes conditional on `OD_XFER_DIRECT_AUTO_END`;
- a fatal transfer calls `od_xfer_app_abort()` again during reset, disconnect or replacement;
- a target's PIPE machine is deleted before that target's § 2.2 rows are closed or its exception
  is recorded;
- a temporary `od_cmd_app_pipe_*` forwarding hook is removed before dispatch reroutes to
  `od_pipe_*` in step 8;
- an interim ratchet is left to fail on an absent function instead of being retired with the code
  it guarded;
- a flush-timing or cadence change rides inside a move commit;
- the reorder queue is sized from anything other than `OD_PIPE_MAX_W` and the protocol constants;
- a reorder-slot `memcpy` is reachable without an immediately preceding, independent
  `payload.n <= OD_PIPE_REORDER_PAYLOAD` guard.

---

## 12. Commit structure

1. § 2 evidence: frozen reference tests, measurements, checklist statements. No production change.
2. `od_xfer_internal.h` PIPE operations + `od_xfer` implementation + direct tests.
3. D4 command-context metadata: `od_cmd_ctx_t`, dispatcher population, the complete mechanical
   fixture-constructor sweep, the two named static-to-runtime fixture demotions, permanent precise
   construction ratchet and focused metadata tests. No PIPE source or route.
4. `od_pipe.{c,h}` dormant, consuming D4 metadata for frame bounds and the minimum-wire-length
   tripwire, and carrying D5's derived geometry/assertions, D9's capability-off arm and the BG22
   map test.
5. The shared PIPE test suite, including the differential comparison and the model traces.
6. ESP32 cutover: deletion, retention, predicate reduction, temporary forwarding hooks,
   disconnect rewiring, ratchet swap.
7. **ESP32 hardware gate.**
8. Nordic cutover: same shape, temporary forwarding hooks, plus frozen test-harness deletion and
   retirement of its context-storage allowance/check.
9. **Nordic hardware gate.**
10. `od_core_reset()` transfer ownership.
11. Dispatch reroute, all temporary/target hook deletion, permanent ratchets.
12. Measurements, checklist and CLAUDE.md status.
13. *(conditional, only if § 2.4 reproduced)* the tail-flush fix, with its own tests and row.

No commit leaves two callable implementations of one opcode. A cutover commit builds with the
replaced source removed.

---

## 13. Definition of done

1. One PIPE machine exists; no target parses `0x80`, `0x81` or `0x82`, owns their accounting,
   constructs their replies or defines a command hook for them.
2. `OD_DISPATCH_OPCODE_ROWS` names `od_pipe_*` directly with unchanged budgets, and
   `od_cmd_app.h` declares no PIPE hook.
3. Target code holds sizing facts, ingress and the `od_xfer_app` hardware seam only.
4. BG22 pays zero PIPE state and still emits its existing refusal bytes.
5. `od_core_reset()` owns transfer reset; no target disconnect path resets transfer state itself.
6. The repository-wide single-pump-owner ratchet is installed and green.
7. Both PIPE-capable targets have passed their § 9 hardware gate, or their open rows are recorded
   against a named unavailable board.
8. Every § 4 decision has a test, and every decision that changes behaviour has a hardware row.
9. `tools/check.sh --targets` reports no failure and no skip.
10. Every direct host fixture constructs `od_cmd_ctx_t` through `od_test_cmd_ctx()`, the permanent
    construction ratchet ignores pointer declarators but admits no untracked object exception, the
    remaining named fixture object is initialized through the helper in `reset_all()`, no allowance
    or paired check names deleted `pipe_write_test.c`, and active DATA refuses a missing/short
    `wire_len` before using it in frame-bound arithmetic.

---

## 14. External follow-up work (not implemented here)

- `opendisplay-protocol`: `OD_ERR_PIPE_START_WRONG_TRANSPORT`, a `RESP_PIPE_WRITE_*` mirror set,
  a canonical constant for the START response's "selective repeat" bit, and an
  `OD_ERR_PIPE_DATA_*` namespace for the `0x02`/`0x03`/`0x04` DATA codes. All four are currently
  raw literals in target code and will be raw literals in shared code. The header is frozen.
- `py-opendisplay`: D6 makes the advertised streaming-decompression bit advisory for PIPE on
  Nordic. Conforming hosts already gate on it, so no client change is required, but the behaviour
  note belongs upstream.
- `docs/FOLLOWUPS.md` § 3.11 records the pre-existing Nordic `0x0052` comment/policy mismatch:
  handler NACKs do stamp activity today. Phase 3 preserves that policy rather than widening into a
  command-wide activity-policy change.
- The sibling repositories are read-only references throughout this phase.
