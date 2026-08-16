# Plan: C11 — retire dispatch scaffolding and close the session/HAL seams

**Status:** proposed execution plan, 2026-08-16.  C10 is landed through `a37c04b`.

**Parent plan:** [`PLAN_OD_DISPATCH_2026-08-14.md`](PLAN_OD_DISPATCH_2026-08-14.md), especially
sections 3, 5 and 7.  This document replaces that plan's one-row C11 description with an
implementation sequence derived from the code that exists after C10.

**Baseline:** C10's final review commit records `tools/check.sh --targets` at 12/0/0.  C9 and C10
have not run on a Nordic board.  The ESP32 C1/C5/C8 stack is likewise still unverified on silicon.
Those are facts this plan carries; a clean build does not retire them.

---

## 1. Outcome

C11 completes the architectural part of the dispatch migration.  At its end:

1. `shared/core/od_dispatch.c` owns the canonical opcode switch as well as validation, reservation,
   gate and outcome mapping.  Each target supplies named per-command handlers; neither target
   supplies a second dispatcher.
2. An ESP32 command carries `{origin, tag}` explicitly from ingress to handler.  The transitional
   `g_commandOrigin`, `g_commandInstance`, `commandOrigin()` and `imageDataWritten()` interfaces are
   gone.
3. `targets/nordic-zephyr/src/opendisplay_pipe.c` owns only BLE ingress, connection-generation
   publication, deferred close cleanup and the bounded RX/TX/producer pump.  Config, NFC, device
   and transfer command policy live behind named target headers.
4. Both session objects are owned by their `od_session_app` translation units.  Other target code
   reaches them through the seam or through narrow compatibility functions, never through an
   exported global or a pipe-internal back door.
5. Nordic PIPE handlers return truthful `od_cmd_result_t` verdicts.  A PIPE NACK can no longer be
   followed by an outer unconditional `OD_CMD_OK`.
6. The three defects deferred by the parent plan are closed:
   - a failed Nordic prepared-key destroy cannot latch the slot until reboot;
   - ESP32 challenge generation uses a fallible CSPRNG API and propagates its failure;
   - a successful `od_session_seal()` stamps `last_activity_ms`.
7. The live status and divergence documents describe C8-C11 rather than their pre-cutover donor
   implementations.

C11 is a structural completion and defect-fix unit.  It does **not** make the direct-write,
partial-write, PIPE or NFC algorithms shared.  Those state machines remain target-owned and become
smaller, explicit inputs to their later promotions.

---

## 2. Corrections to the parent plan

The parent plan was accurate when C8 had not started.  Four details are stale after the landed
work and must not drive C11 blindly.

### 2.1 C10 is not one cutover commit anymore

C10 is the series `191627d..a37c04b`.  Its review already removed Nordic's local handshake,
decrypt scratch, confidentiality heuristic and inline notification retry.  C11 must not recreate
those changes under a cleanup label.  It works from the post-review implementation.

### 2.2 The fake-HAL prerequisite is already satisfied

`522ad60` added fail-after-N crypto injection and covers every step-2 session failure, including
the all-zero session id.  C11 extends those tests for the seal activity stamp; it does not schedule
the fake-HAL work again.

### 2.3 “Remove dead egress helpers” now means a residue audit

The ESP32 sender ring and `sendResponse*` family are already gone.  The concrete residue is:

- unused nonce-log state still in `communication.cpp` after reporting moved to
  `od_session_app.cpp`;
- an opaque, misleading `imageDataWritten()` wrapper with unused BLE-shaped parameters;
- current-origin/current-instance globals used to reconstruct a reply context that ingress already
  knows;
- comments in display code that still describe `sendResponse()` and byte-inferred sealing.

Delete only symbols proven unused after the new calls are live.  Do not turn “cleanup” into an
unreviewable rewrite of the remaining config handlers.

### 2.4 The session/HAL findings are three different fixes

The Nordic slot lifecycle and ESP32 random source are target HAL fixes.  The activity stamp is a
shared `od_session` semantic fix.  They get separate tests and review points even if they land in
one C11 series.

---

## 3. Invariants C11 may not move

These are acceptance criteria, not explanatory comments:

- Dispatch order remains:
  `structure -> live tag -> producer conflict -> budget -> reserve -> gate/decrypt -> handler`.
- `OD_FRAME_DEFERRED` remains possible only before decrypt and leaves the RX head byte-identical.
- Every caller of `od_dispatch_frame()` calls `od_core_frame_done()` exactly once.
- Every reservation is released exactly once; a producer may consume/transfer its unit as today.
- Plain/protected selection remains explicit at the reply call site.  No replacement helper may
  infer confidentiality from bytes 0 or 2.
- A replay/out-of-window encrypted `0x81` is logged before silence, consumed, and does not change
  the PIPE transfer or integrity-strike count.
- Config save/clear ACKs are committed while the session that authorized them still exists; config
  reload and session clear happen afterwards.
- A successful key-loss rewrite remains plaintext by necessity.  A transfer cannot change its
  authenticated/unauthenticated class between opener and continuation.
- END ACKs meet the bounded flush barrier before a blocking refresh.  Timeout leaves the entry
  queued; it never drops it.
- Nordic's main pump remains bounded to one RX ring per pass.
- Disconnect cleanup stays on the consumer/main context.  The BT callback only advances the
  generation and publishes pending cleanup.
- Existing capability behavior is preserved except the already-decided Nordic `CMD_POWER_OFF`
  unsupported NACK described below.  ESP32 NFC remains unknown/silent; Nordic deep sleep remains
  recognized/silent; PIPE over ESP32 LAN remains a post-gate NACK.

---

## 4. Target architecture after C11

### 4.1 Shared dispatcher owns the opcode map

Add `shared/core/od_cmd_app.h`, the link-time target seam.  It declares one function for each
canonical command that reaches a target handler; AUTHENTICATE remains wholly owned by `od_gate`.
Use command-domain names rather than one generic callback, for example:

```c
od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_cmd_app_direct_start(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_cmd_app_pipe_data(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_cmd_app_nfc(const od_cmd_ctx_t *ctx, od_span_t body);
```

The complete list follows the canonical switch currently duplicated in
`targets/esp32-idf/src/od_cmd_app.cpp` and
`targets/nordic-zephyr/src/opendisplay_pipe.c`.  Unsupported target capabilities still implement
their hook and return `OD_CMD_UNKNOWN`, unless the protocol defines an explicit unsupported NACK.
This is static link-time composition: no registry, vtable, constructor or heap allocation.

`od_dispatch.c` adds one internal `dispatch_plain()` switch over those functions.  The existing
post-handler result mapping stays in one place.  `od_cmd_dispatch()` is then deleted from
`od_dispatch.h` and from both targets.

Keep `od_cmd.h` for the types that are genuinely shared (`od_cmd_result_t`, `od_frame_outcome_t`,
`od_cmd_ctx_t`, `od_frame_policy_t`).  Remove its inaccurate promise that declarations it no
longer contains will disappear with transfer promotion.  The target callback declarations belong
only in `od_cmd_app.h`, whose header comment explicitly calls them a migration seam.

### 4.2 Capability decisions in the shared switch

The switch moves opcode ownership, not capability policy:

- ESP32's NFC hook returns `OD_CMD_UNKNOWN`; do not manufacture an unsupported NFC error code.
- Nordic's `CMD_POWER_OFF` hook emits the canonical four-byte unsupported response
  `{0xFF, 0x52, OD_ERR_POWER_OFF_UNSUPPORTED, 0x00}` and returns `OD_CMD_NACK`.  This is the one
  deliberate wire change already accepted by parent-plan decision D-E.  `FOLLOWUPS.md` still owns
  the contradictory three-byte text elsewhere in the canonical header; C11 does not edit the
  vendored protocol copy.
- Nordic deep sleep keeps its recognized/silent behavior.
- ESP32 PIPE-over-LAN refusal stays inside the PIPE hook, after the shared gate.
- Unknown opcodes still produce no response and no activity stamp.

Add a host routing table test that invokes every canonical opcode and records exactly which fake
hook ran.  It must include the two capability exceptions above and an unknown opcode.  A new opcode
must fail this test until its route and capability behavior are chosen.

### 4.3 ESP32: remove implicit frame context

Replace `imageDataWritten()` with a target helper whose only inputs are the actual dispatch inputs:

```c
od_frame_outcome_t od_dispatch_app_frame(const od_reply_t *rp,
                                         uint8_t *frame, uint16_t len);
```

It may retain the ESP32 command banner and reservation-leak assertion, but it must not reconstruct
`rp` from globals.  BLE creates `{OD_ORIGIN_BLE, item->tag}` from the RX slot; LAN creates
`{OD_ORIGIN_LAN_PLAIN/TLS, linkIdWord(lanOwner)}` at the socket parser.  Both call the helper
directly.

Thread origin through the handlers that need it:

- `configWriteGate(ctx)` reads `ctx->rp.origin` instead of `g_commandOrigin`;
- display transfer START records `ctx->rp.origin`;
- DATA/END ownership checks compare `ctx->rp.origin` with the recorded transfer origin;
- replies already use `ctx->rp`, so no replacement global is permitted.

After both ingress paths compile and the origin tests pass, delete:

- `g_commandOrigin`;
- `g_commandInstance`;
- `commandOrigin()`;
- `imageDataWritten()` and its opaque `BLEConnHandle`/`BLECharPtr` typedefs if no other interface
  needs them.

Add a boundary grep for those four names.  This is more than tidiness: a frame context stored in a
global can be read by a nested or future asynchronous path after the caller has changed it.

### 4.4 Nordic: split policy out of `opendisplay_pipe.c`

The resulting ownership is:

| File | Owns |
|---|---|
| `opendisplay_pipe.c` | RX callback, connection generation, pending-close flag, stale discard, bounded core pump |
| `od_session_app.c` | `s_session`, hardware-id packing, clock/config/report seam |
| `od_cmd_device.c` | version, MSD, reboot, DFU, power-off, deep-sleep, LED, buzzer hooks |
| `od_cmd_config.c` | config read/start, write/chunk/clear state, rewrite authorization and reset |
| `od_cmd_direct.c` | direct/partial handler adapters and refresh-barrier calls |
| `od_cmd_nfc.c` | NFC response buffer and chunked NFC-write state |
| `opendisplay_pipe_write.cpp` | existing PIPE algorithm, now returning truthful verdicts |
| `od_hal_radio.c` | one notify attempt and tag liveness |

Names may be adjusted to local convention, but state must move with its handlers; do not expose
file-static state through a general “internal” struct.

Delete the single-connection `connection` fields from config and NFC chunk state and delete their
tautological `== 0` checks.  Delete `s_long_write_len` and `s_long_write_conn`, which C10 only resets
and never reads.  Each stateful command module exports a narrow reset function used by deferred
disconnect cleanup.

`opendisplay_pipe_internal.h` must lose the session and device-id accessors.  If connection
generation remains its only member, rename it to describe that ownership or fold the accessor into
the pipe public target header; it must not remain a generic escape hatch.

### 4.5 PIPE handlers return verdicts

Change Nordic's three functions to return `od_cmd_result_t`:

```c
od_cmd_result_t opendisplay_pipe_write_start(...);
od_cmd_result_t opendisplay_pipe_write_data(...);
od_cmd_result_t opendisplay_pipe_write_end(...);
```

Every path that emits a hard NACK returns `OD_CMD_NACK`; accepted ACK paths return `OD_CMD_OK`.
Paths where `od_reply()` substitutes a hard NACK stop the transfer as C10 already requires and
return NACK.  The outer per-command hooks return those verdicts directly—no unconditional OK tail.

Do not change reorder-window, SACK, auto-complete, refresh or abort behavior in this step.  A host
harness should compile the production PIPE implementation against fake display/reply/Zephyr seams
and pin at least: bad START, cadence ACK, gap NACK, auto-complete, explicit END and reply-substitution
abort.

### 4.6 Session objects live behind `od_session_app`

Nordic:

- move `s_session` and the exact hwinfo-to-four-byte packing into `od_session_app.c`;
- make config handlers and disconnect cleanup call `od_session_app_state()` rather than retain a
  second accessor;
- retain `od_session_clear()`, never `memset`, for all teardown.

ESP32:

- move `g_session` into `od_session_app.cpp` as file-static storage;
- make the compatibility functions in `encryption.cpp` delegate through
  `od_session_app_state()`;
- make `od_core_frame_done()` use the seam;
- remove `g_session` from `encryption_state.h`.  Keep that header only if its config globals still
  justify it; otherwise replace it with their real owning header.

The four-byte device identity is wire-visible.  Move the implementation, not the arithmetic, and
add a target-level deterministic packing test around a supplied eight-/six-byte hardware id so the
refactor cannot silently identify the same board differently.

### 4.7 Add the missing common reset primitive

The parent API and several shared comments already name `od_core_reset()`, but no symbol exists.
Add it to the APP_SESSION tier with the consumer-context-only contract:

```c
void od_core_reset(void); /* cancel config producer, reset TX queue, clear app session */
```

It performs only shared ownership cleanup:

1. `od_config_read_cancel()`;
2. `od_txq_reset()`;
3. `od_session_clear(od_session_app_state())`.

Target transfer/display/config/NFC reset remains target-owned and runs adjacent to this call.
`od_core_reset()` does not reset RX because Nordic's producer may race on the BT thread and ESP32's
connection policy has stricter ordering around its ring.  It is never called directly from a
stack callback.

Adopt it in Nordic deferred-close cleanup and in the shared portion of ESP32's abort path.  Config
reloads that intentionally clear only the session continue to call `od_session_clear()`; using the
full reset there would discard already-sealed config ACKs.

---

## 5. Deferred defect fixes

### 5.1 Nordic prepared-slot destroy failure

Current failure: `slot_release()` clears `s_slots[slot]` and `s_slot_ready[slot]` only after
`psa_destroy_key()` succeeds.  A single destroy error leaves the slot marked ready, so every later
`key_set()` retries the same failing release and authentication remains unavailable until reboot.

Fix the ownership before invoking PSA:

1. copy the key id to a local;
2. clear the tracked id and ready flag;
3. call `psa_destroy_key(local_id)`;
4. log and return ERROR on failure, explicitly recording that the PSA slot was leaked.

The handshake that encountered the failure may fail.  The next handshake must be able to import a
new key.  Do not retain the failed id for retry: PSA may later reissue it, making a delayed destroy
target somebody else's key.

Add a host test that compiles the production Nordic HAL against a fake PSA API.  Force one destroy
failure and assert:

- the first replacement reports ERROR;
- a second replacement imports successfully rather than retrying the old destroy forever;
- clear after a failed destroy is idempotent;
- no call attempts to destroy a reused/fresh id accidentally.

### 5.2 ESP32 random generation must be fallible

`esp_fill_random()` returns `void`, so the current HAL always reports success and cannot implement
the shared contract's “never offer a challenge the device cannot honor” failure path.

Use Mbed TLS PSA's `psa_generate_random()` for this one operation.  It is already supplied by the
pinned `mbedtls` component and returns `psa_status_t`.  Initialize PSA once in the random adapter,
propagate init/generation failure as `OD_HAL_CRYPTO_ERROR`, log the status, and leave the caller's
challenge path to synthesize the existing `AUTH_STATUS_ERROR` response.

Keep the existing mbedTLS CCM/CMAC/AES implementations; C11 is not a second crypto-backend swap.
Isolate the random adapter if necessary so a host test can compile the production function against
fake PSA calls and assert init failure, generation failure, success and null-argument behavior.
Do not fall back silently to `esp_fill_random()` after a PSA error.  If the pinned IDF configuration
cannot link `psa_generate_random()` without enabling a new global crypto mode, stop and revise this
decision rather than landing an unreviewed sdkconfig change.

Measure flash and static RAM before/after on one ESP32 board.  A material PSA runtime cost for a
16-byte challenge is a finding, not permission to hide it.

### 5.3 Successful seal stamps activity

Set `s->last_activity_ms = now_ms` only after the cipher succeeds and `out_len` is final.  Do not
stamp on `NO_SESSION`, preflight size errors, counter exhaustion or crypto failure.  A cipher error
may have spent a counter, but it did not produce outbound activity.

Extend `session_test.c` to pin every side of that boundary:

- successful seal updates to exactly `now_ms`;
- TOO_SHORT, TOO_LONG and NO_ROOM do not update;
- NO_SESSION and COUNTER_EXHAUSTED do not update;
- forced crypto failure does not update;
- open and explicit `od_session_touch()` retain their existing behavior.

The target's later `od_core_frame_done()` touch is harmlessly idempotent.  Do not remove it: an
accepted TLS-LAN command bypasses CCM and needs the target policy stamp.

---

## 6. Commit sequence

Use a C11 series rather than one mixed commit.  Each commit must be reviewable and leave every
target link-complete.

| Commit | Content | Required proof |
|---|---|---|
| **C11.0** | Add failing host tests/fixtures for slot recovery, fallible ESP RNG, seal activity and per-opcode routing | Fail for the named reason; no production behavior changed |
| **C11.1** | Fix Nordic slot lifecycle, ESP random adapter and shared seal stamp | Focused tests green; host suite gcc/clang/sanitizers; all target builds |
| **C11.2** | Introduce `od_cmd_app.h`; move the canonical switch into `od_dispatch.c`; convert both targets to named hooks | Routing matrix green; no `od_cmd_dispatch` implementation remains; dispatch mutations caught |
| **C11.3** | Return truthful verdicts from the existing Nordic PIPE files, with no file movement | PIPE harness green; Nordic builds; state-machine and wire bytes unchanged |
| **C11.4** | Move Nordic command domains and their state out of `opendisplay_pipe.c`, mechanically | Before/after symbol inventory; Nordic builds; no behavioral diff |
| **C11.5** | Remove ESP32 implicit origin/instance globals and thread `od_reply_t` through BLE/LAN and transfer ownership | BLE/LAN origin tests; boundary grep; all ESP32 fragments and sdkconfig baseline |
| **C11.6** | Move both session objects behind `od_session_app`; add/adopt `od_core_reset`; delete proven dead residue | Reset tests, disconnect-race test, target builds, no exported session singleton |
| **C11.7** | Documentation/status corrections and final ratchets | `git diff --check`; full gate 12/0/0; hardware results or explicit ACCEPTED-UNRUN debt |

If a structural step exposes a behavior defect, fix it in a separate review commit before
continuing.  Do not widen a response budget or add a reply merely to make a test pass; report the
existing behavior first.

---

## 7. Automated verification

### 7.1 Focused host coverage

- Existing session, gate, reply, dispatch, TXQ, RXQ and config-read suites remain green.
- New session cases pin success-only seal activity.
- New target-HAL lifecycle tests use production source with fake vendor APIs; copied logic is not a
  test of the implementation.
- New dispatch routing cases cover every opcode hook, unsupported capability and unknown opcode.
- New Nordic PIPE verdict cases cover each multi-reply and substitution branch.
- Existing RX threaded reset race remains part of the gate.

Mutation checks required before C11.7:

1. restore “clear tracking only on successful PSA destroy” — slot test fails;
2. make ESP random always return OK — random failure test fails;
3. remove the seal activity assignment — session test fails;
4. route any opcode to its neighbor's hook — routing test fails;
5. force any Nordic PIPE NACK path to return OK — verdict test fails;
6. make `DEFERRED` consume RX — existing dispatch/RX test fails.

### 7.2 Full software gate

Run `tools/check.sh --targets` after every accepted implementation commit and once from a clean
tree at the final C11 candidate.  Required summary: zero failures and zero skips.  This includes:

- boundary greps;
- gcc, clang and ASan/UBSan host suites;
- pre-auth fuzz targets;
- wire corpus;
- ESP32 shim ratchet, all board fragments and sdkconfig baselines;
- all three Nordic board builds.

Record the host check count and Nordic RAM delta.  C10 added about 8 KB for `od_txq`; C11 should
mostly move code, so an unexplained second large increase is a blocker.

### 7.3 Structural ratchets

The final tree must satisfy:

- no target definition of `od_cmd_dispatch`;
- no `g_commandOrigin`, `g_commandInstance`, `commandOrigin` or `imageDataWritten`;
- no exported `g_session`, `od_pipe_session` or `od_pipe_device_id`;
- no Nordic command-handler definitions in `opendisplay_pipe.c`;
- no void Nordic PIPE command API;
- no session/nonce logging copy in `communication.cpp`;
- no byte inspection used to choose `od_reply()` versus `od_reply_plain()`.

Use boundary checks for the names whose absence is architectural.  Do not ratchet a line count;
ratchet ownership and symbols.

---

## 8. Hardware gates

### 8.1 Entry gate

Highest-value sequence before C11 implementation:

1. Flash C10 (`a37c04b`) on `xiao_nrf52840` and run the full C10 matrix.
2. Smoke ESP32-S3 through authentication, encrypted command, upload and disconnect/reconnect.

This separates a C9/C10 transport or cutover failure from C11's adapter cleanup.  If no board is
available, implementation may proceed, but the result is **ACCEPTED-UNRUN**, not hardware-verified,
and C12 inherits the full stacked debt explicitly.

### 8.2 C11 exit matrix

Nordic (`xiao_nrf52840` minimum):

- authenticate, encrypted command and re-authenticate after disconnect;
- config single/chunk write, sealed ACK, reload, read-back and reboot persistence;
- config read under induced TX backpressure;
- direct and PIPE END ACK visible before physical refresh;
- induced encrypted PIPE replay/reordering: log, silence and recovery;
- NFC read at 218 bytes and error at 219;
- `0x52` returns the four-byte unsupported NACK;
- unknown opcode stays silent.

ESP32-S3:

- two fresh authentications exercise PSA random generation;
- BLE encrypted command and PIPE smoke;
- plaintext and TLS-LAN command paths still carry the correct explicit origin;
- PIPE over LAN remains refused after the gate;
- BLE disconnect/reconnect cannot dispatch or deliver a stale tag;
- config write ACK remains sealed before session reload.

A real PSA destroy fault is not practical hardware acceptance; the fake-PSA lifecycle test owns
that injected branch.  Hardware owns successful key replacement and confirms the fix did not break
the normal PSA lifecycle.

---

## 9. Documentation updates

C11.7 is a documentation-only commit that updates:

- `CLAUDE.md` status: both command paths consume shared dispatch/egress; exact hardware debt and
  measured Nordic RAM;
- `docs/DIVERGENCE_MATRIX.md` section 1 in full, not only rows 1.5b/1.7—the old queue sizes,
  dispatch locations, gate descriptions and line references are all stale;
- `docs/SHARED_API_DESIGN.md`: target handler seams and the real `od_core_process` composition;
  correct the claim that every disabled NFC implementation NACKs;
- `docs/OD_SESSION.md`: close the slot latch and seal-activity items, record ESP random backend and
  current hardware status;
- parent dispatch plan: mark C8-C11 landed with commit range and point future work at C12;
- `docs/FOLLOWUPS.md`: retain the protocol-level nonce-reuse and `0x52` width contradictions; do
  not claim firmware cleanup resolved either protocol defect.

No document may call a target hardware-verified from build or host evidence.

---

## 10. Risks and stop conditions

- **Opcode switch movement can change “unknown” into “recognized.”** The routing matrix and explicit
  capability hooks are mandatory.  Stop on any unplanned response to a formerly silent opcode.
- **Origin-global removal touches LAN and BLE simultaneously.** Do not delete the globals until both
  callers pass explicit contexts and transfer-origin checks use them.
- **Session movement can change device identity.** Preserve byte packing exactly and test it before
  moving the implementation.
- **PSA RNG may alter ESP memory/configuration.** Stop if it requires a global sdkconfig change or
  material runtime allocation; revise the backend decision explicitly.
- **Verdicts are policy.** A wrong OK/NACK changes activity classification even when response bytes
  are unchanged.  Derive each verdict from the path's actual wire action and state mutation.
- **Cleanup can hide behavior changes.** File movement and semantic changes land separately.  A
  moved handler should be byte-for-byte first; verdict or protocol changes follow with tests.
- **C9/C10 hardware debt makes diagnosis ambiguous.** Prefer the entry gate.  If skipped, preserve
  the exact debt in status and do not use C11's green software gate as evidence about radio timing.

Stop and revise the plan if any step requires a new opcode/error code, changes encryption envelope
bytes, promotes a transfer algorithm into `shared/`, or changes Silabs.  Those are distinct units.

---

## 11. Definition of done

C11 is complete only when:

- the canonical opcode switch exists once in shared dispatch;
- both targets link through named command hooks and truthful outcomes;
- frame origin/tag is explicit end-to-end on ESP32;
- Nordic's pipe file is transport/pump code rather than a second command subsystem;
- both session objects are private to their app seams;
- the prepared-slot latch, fallible ESP RNG and seal activity findings have tests and fixes;
- reset and disconnect preserve queue/producer/session ownership;
- the full software gate is 12/0/0 from a clean tree;
- documentation distinguishes software proof from hardware proof; and
- hardware matrices are recorded, or the result is explicitly labelled ACCEPTED-UNRUN for C12.
