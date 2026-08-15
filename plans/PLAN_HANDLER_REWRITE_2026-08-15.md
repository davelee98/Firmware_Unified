# Plan: move ESP32 handlers onto `od_reply`, completing C8's vertical adoption

**Status:** revision 2, 2026-08-15. Not started. Revision 1's sequence was unsound — see
§ "Corrections to revision 1".
**Parent:** [`PLAN_OD_DISPATCH_2026-08-14.md`](PLAN_OD_DISPATCH_2026-08-14.md) § 3.2, § 3.6, C8.
**Execution gate:** the ESP32-S3 `od_session` Gate 2 must close first. This plan rewrites the
reply path of every handler on a target whose session swap has never run on silicon; landing it
first makes a hardware failure ambiguous across the session swap, the mbedTLS arm, the dispatcher,
the queue, and 78 rewritten call sites at once.

## The problem

C8's shared core is landed and mutation-tested, but **nothing on the target routes through it**.
`od_txq`'s reservation accounting is decorative on ESP32 today: every handler replies via
`sendResponse()`, which writes to the old 10-slot ring with no reservation and no origin.

The two ends do not meet:

- `od_reply(r, rp, frame, len)` needs a reservation token and a reply context, both per-dispatch.
- `sendResponse(response, len)` has neither, and 78 call sites across five files call it.

The dispatch plan forbids the cheap bridge explicitly — "handlers take the reply context
explicitly, **no current-origin global**" — because a file-static "current reservation" reproduces
exactly the ambiguity `od_reply_t`-by-value exists to remove, and because a config producer and an
inbound command can be in flight across loop passes at the same time.

## Scope, measured

| File | `sendResponse*` sites | Notes |
|---|---:|---|
| `display_service.cpp` | 30 | transfer handlers; THREE reply across a blocking refresh |
| `communication.cpp` | 25 | includes the two entry points themselves and the gate's own answers |
| `device_control.cpp` | 11 | LED, power, deep sleep |
| `buzzer_control.cpp` | 10 | all in one handler |
| `encryption.cpp` | 2 | the handshake reply, already routed through `od_gate` in the new path |
| **Total** | **78** | |

Two facts that shape the sequence, both verified rather than assumed:

- **No handler replies from an asynchronous context.** Every site is reached synchronously from
  the dispatch that owns it, including `directWriteFinishAndRefresh()`, which is a static helper
  called from handlers rather than a timer or callback. So a reservation passed down the call
  chain is always the right one; no site needs a deferred or borrowed token.
- **THREE paths reply across a blocking refresh, not one.** `directWriteFinishAndRefresh` is the
  obvious one; direct-partial END and PIPE-partial END also block in `partial_write_to_panel()` ->
  `waitforrefresh(60)` and reply afterwards. Each holds its reservation for the duration, which is
  free under § 3.2 — capacity is a counter, not a parked slot — and is the reason the counter
  design was chosen. None of the three drains before the refresh today, so all three need the
  step-7 barrier.

## Design

### The context travels as one parameter, not two

```c
typedef struct {
    od_reply_t           rp;    /* by value: origin + tag */
    od_tx_reservation_t *r;     /* by pointer: units are spent as replies are made */
} od_cmd_ctx_t;

od_cmd_result_t handleX(const od_cmd_ctx_t *ctx, od_span_t body);
```

**One parameter rather than passing `rp` and `r` separately**, because 78 sites is enough that a
transposed pair would happen, and because the compiler cannot catch it — both are pointers. It
also gives the later per-opcode split (dispatch plan C11) something stable to shrink toward.

`rp` is a copy and `r` is a pointer: the reply context is immutable for the dispatch, the
reservation is consumed by it. Making both pointers would invite a handler to hold `ctx` past its
dispatch, which is exactly what the token generation added in `ecef843` exists to catch.

### `sendResponse()` becomes two explicit calls, and then disappears

Every site is classified as it is converted — this is the § 3.6 rule arriving at the handlers:

- `od_reply_plain(ctx->r, &ctx->rp, ...)` for control and error frames: auth-required,
  decrypt-failure, every hard NACK, AUTHENTICATE, FIRMWARE_VERSION.
- `od_reply(ctx->r, &ctx->rp, ...)` for application responses, **including all PIPE ACKs**.

The classification is not mechanical and must not be automated: the current code infers it from
the response bytes, and that inference is the defect § 3.6 removes. **Every site is read and
decided.** A site whose classification is not obvious from its handler is a finding, not a
judgement call to make quickly.

`sendResponse()` and `sendResponseUnencrypted()` are deleted at the end of the sequence, not
kept as shims. A shim that still compiles is a route back to the ambiguity.

### Return values

Handlers currently return `void` and signal failure by the bytes they send. They gain
`od_cmd_result_t`:

- `OD_CMD_OK` — normal completion, including a handler that sent a NACK for a bad argument.
- `OD_CMD_NACK` — the frame was refused. Still activity (§ 5): the client is talking correctly.
- `OD_CMD_AUTH_REJECTED` — the handler itself refused for want of authentication
  (`handleWriteConfig`'s own gate). Distinct because only this advances the abuse run, and
  collapsing it lets a TLS client's refused `CONFIG_WRITE` hold the exclusive link forever.

## Corrections to revision 1

Revision 1's sequence **did not work**, and the claim it rested on was false. It asserted that
steps 2-6 leave the target working because "a converted handler still reaches the old ring". It
does not: `od_reply()` commits exclusively to `od_txq`, nothing drains `od_txq` until the cutover,
and so **every converted command would stop answering from step 2 onward**. The target would still
build, which is what made the error easy to write and impossible to notice from a build.

Two further errors in revision 1, both found by independent review:

- It claimed the per-opcode budget table was "already asserted" in `dispatch_test.c`. Only `0x81`
  and one ordinary opcode were. Every multi-reply opcode is pinned now.
- It treated the parent plan's budget table as given. **`0x82` was wrong**: an explicit PIPE END
  emits the tail SACK, then the END ACK, then the refresh status — three replies, not two. At two
  the third is dropped *after the panel has already refreshed*, so the host loses the completion
  signal for work the device did. Fixed in `od_dispatch.c` and pinned. The `od_txq_reserved() == 0`
  assertion revision 1 proposed would NOT have caught it: releasing an exhausted token leaves zero.

Also fixed while here, in already-landed code: `od_config_read_start()` sent its hard NACK through
`od_reply()` rather than `od_reply_plain()`, so a config-load failure would be sealed and
unreadable to a client whose session had just died — the same class as the Nordic regression that
sealed its own rejection frames.

## Sequence

Each step builds and passes `tools/check.sh --targets`.

**The egress switch must be atomic, but the 78-site rewrite must not be.** Running both rings at
once is unsafe rather than merely untidy: under a BLE `RETRY`, a later reply in one ring can
overtake an older reply stalled in the other, and response order is load-bearing for PIPE. So the
sequence separates *threading the context through* (incremental, always working) from *changing
which egress is live* (one atomic step).

| Step | Content |
|---|---|
| **1** | `od_cmd_ctx_t` in `od_cmd.h`. A temporary `od_cmd_reply{,_plain}(ctx, ...)` adapter that makes the § 3.6 classification explicit at the call site but **routes to the legacy sender**. No behaviour change. |
| **2** | `buzzer_control.cpp` (10). The smallest complete file: it proves the shape end to end before the shape is applied 68 more times. |
| **3** | `device_control.cpp` (11). |
| **4** | `communication.cpp`'s handler sites — **not** the two entry points, which step 8 removes. `handleReadConfig()` keeps its synchronous loop here and converts only its reply CALLS; it becomes `od_config_read_start()` at step 8, in the commit that adds the pump. Revision 1 put the producer swap here, which would have broken config read immediately: the producer emits chunk 0 and then needs `od_config_read_pump()`, which nothing calls until the cutover. |
| **5** | `display_service.cpp` (30). Largest, and the one holding a reservation across a refresh. |
| **6** | Handlers return `od_cmd_result_t`; `od_cmd_dispatch()` is implemented over the existing switch; `od_cmd_mutates_config()` lands. Still legacy egress. |
| **7** | The pre-refresh barrier, target-side: one helper used by **every** END path, calling `od_txq_flush()` repeatedly against a deadline while feeding the watchdog. Still legacy egress, so it is a no-op until step 8 — but it is written and reviewed before the cutover rather than during it. |
| **8** | **CUTOVER, atomic.** The adapter's routing flips to `od_reply*`; `imageDataWritten()` calls `od_dispatch_frame()`; the loop calls `od_txq_process()` **and `od_config_read_pump()`**; `od_core_frame_done()` applies `od_frame_policy()`; both ingress paths honour `consume_rx == false`; teardown calls `od_config_read_cancel()` and `od_txq_reset()`. `sendResponse`, `sendResponseUnencrypted`, `queueBleNotifyCopy` and the 10-slot ring are deleted, and so is the adapter's legacy branch. |

Steps 1-7 leave the device running the shipped egress the whole way, so each is revertable on its
own and none can be observed from the wire. Step 8 is the first moment the new path carries a
frame, and is a Gate 2 event.

## What step 8 must wire, in full

Revision 1 listed only `od_txq_process()`. That is not enough to have a working device, and each
omission below is silent rather than loud:

- **`od_config_read_pump()` in the loop.** `od_config_read_start()` emits chunk 0 and nothing else;
  without the pump a config read stops after 100 bytes and stays active forever, which then defers
  every config write behind it permanently.
- **Both ingress paths must honour `consume_rx == false`.** BLE consumes the ring head after
  `imageDataWritten()` (`main.cpp:887`) and LAN removes the frame from its socket buffer
  (`wifi_service.cpp:1627`), both unconditionally. A `DEFERRED` frame would therefore be dropped
  rather than retried — which turns backpressure and producer conflicts into silent command loss,
  the exact failure `DEFERRED` exists to avoid.
- **Teardown cancels the producer and resets egress.** A client that disconnects mid-read
  otherwise leaves the producer active forever, deferring every subsequent config write.
- **LAN-origin responses need draining too**, not only a drain after BLE RX.
- **`handlePowerOffCommand` needs a bounded flush before `powerLatchPowerOff()`.** The shipped
  sender writes a LAN reply straight to the socket (`communication.cpp:397`), so a LAN client's
  `0x0052` ack is delivered today. After the cutover it sits in `od_txq`, and the loop that would
  drain it never runs again — a regression from "delivered" to "never sent". BLE is unaffected
  because that ack was already best-effort.

## Tests

Host-side, landing with the step that introduces the code:

- **Per-file conversion tests are not proposed.** The handlers are target code with target
  dependencies (panel, NVS, GPIO); a host test of `handleBuzzerActivate` would be testing a fake.
  What IS host-testable, and is already covered, is everything they call into: `od_reply`'s
  seal-or-plain choice, `od_txq`'s accounting, `od_dispatch`'s ordering, `od_frame_policy`.
- **Step 8 adds one target-side assertion**: after each dispatch, `od_txq_reserved() == 0`. It
  catches a LEAKED unit. It does NOT catch an over-budget handler — releasing an exhausted token
  leaves zero either way — so it is not a substitute for the table below.
- The **per-opcode budget** is now pinned in `dispatch_test.c` for every multi-reply opcode
  against a fake handler emitting exactly its worst case. Revision 1 claimed this was already
  true; it was not, and the gap hid a wrong `0x82`. Any handler found to exceed its opcode's
  budget is a finding against the TABLE, not a licence to raise it silently.

## Hardware

Step 7 is a Gate 2 event in its own right, on top of the `od_session` pass this plan is gated on.
The full matrix from the dispatch plan § 9 applies. The three that specifically exercise what this
plan changes:

- **Direct-partial END and PIPE-partial END**, not only the full-frame helper. Both block in
  `partial_write_to_panel()` -> `waitforrefresh(60)` and neither drains first today, so both need
  the step-7 barrier and both are acceptance items. Revision 1's claim that only one handler
  replies across a blocking refresh was wrong.
- **A config read larger than the queue** — the resumable producer under real backpressure, which
  is where truncation would appear.
- **PIPE at a small `ack_every`** — the 3-slot budget for `0x81`, where under-reservation shows up
  as a lost ACK mid-upload.
- **END ACK on air before the physical refresh begins**, transport permitting — the barrier, and
  the reservation held across 60 s.

## Risks

1. **Step 7 is a big-bang cutover and cannot be made smaller.** `od_dispatch.c` currently links on
   ESP32 only because `--gc-sections` drops it; the first call turns the missing seams into link
   errors. Mitigated by steps 2-6 doing all the per-site judgement first, so step 7 is wiring
   only — but it is still the first moment the new path carries a real frame.
2. **78 classification decisions, each individually cheap and collectively the whole risk.** A
   response wrongly given to `od_reply_plain` leaks payload that should have been sealed; wrongly
   given to `od_reply` becomes unreadable to a client whose session just died. Neither is caught
   by a build. Mitigated by converting one file at a time and by `od_reply`'s own refusal to seal
   a frame with no session.
3. **A handler that exceeds its opcode's budget** is currently invisible — it just calls
   `sendResponse` again. After conversion it gets `OD_TXQ_INVARIANT` and drops the frame. Any such
   handler is a pre-existing defect this plan exposes rather than causes, and must be reported
   rather than fixed by widening the budget.
4. **Unknown opcodes cannot currently be reported.** `od_cmd_result_t` has no member for them, so
   the shared dispatcher can only return ACCEPTED, HANDLER_NACK or AUTH_REQUIRED after calling the
   target switch — meaning an unknown opcode would STAMP ACTIVITY and reset the abuse run,
   contrary to § 5. `OD_CMD_UNKNOWN` must be added and mapped to `OD_FRAME_UNKNOWN_OPCODE` before
   step 6, with both the silence and the no-activity policy pinned.
5. **STILL OPEN after step 5: `od_reply()`'s non-OK results are ignored at every converted site.**
   Every call is `(void)od_cmd_reply...`, which is harmless while the adapter routes to the shipped
   sender and cannot fail, and becomes a defect at the cutover. The multi-reply refresh paths are
   the sharp ones: if sealing the END ACK fails, od_reply queues a plaintext hard NACK, and the
   handler then refreshes the panel and queues a contradictory success behind it. Step 8 must make
   these paths stop emitting after a substitution -- while deciding separately whether committed
   panel work should still proceed, which is a different question from what to put on the wire. It can substitute a plaintext
   hard NACK and return `TOO_LARGE`/`SEAL_FAILED`, after which the caller must not reply again. A
   straight replacement of `void sendResponse()` calls would drop that on the floor, and a
   multi-reply path could then queue a contradictory success after a substituted NACK. The
   adapter returns `od_txq_status_t` and multi-reply flows must stop emitting after a
   substitution — while deciding separately whether committed panel work should still proceed.
6. **`handleReadConfig`'s conversion is not a conversion** — it becomes a different mechanism
   (`od_config_read_start` + the pump). Its old form drains the ring between chunks by blocking
   the loop task, which the new path cannot do. Step 4 is therefore the largest single behavioural
   change outside step 7.

## Out of scope

The per-opcode handler split (`od_cmd_led_activate` etc.) stays in dispatch-plan C11. Nordic is
C10. LAN egress beyond what `od_hal_radio` already routes is unchanged. No opcode, error code, or
frame shape changes anywhere in this plan — if one appears to be required, that is a finding.
