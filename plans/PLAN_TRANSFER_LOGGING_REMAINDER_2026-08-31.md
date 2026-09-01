# Transfer Logging Remainder Plan

**Date:** 2026-08-31  
**Status:** PLANNED, not implemented  
**Parent:** [Logging Convergence Plan](PLAN_LOGGING_CONVERGENCE_2026-08-30.md)

This document is the sole implementation and status record for the transfer-logging remainder.
The parent plan retains the completed Stages 0a-9 and its original L4 audit checklist.

## 1. Status and authority

**Status: PLANNED, not implemented.** This follow-on closes the four unchecked L4 items in the
parent convergence plan's § 5.
It is not a Stage 10 of the original convergence sequence: Stages 0a-9 remain complete, while the
steps below are named R0-R3 (remainder) so their status cannot be confused with what already
landed.

The audit was performed against this repository at `42ecda0`, live `../Firmware` at `21befa1`,
and live `../Firmware_NRF54` at `0f19c0c`. The shared state machines here are the implementation
authority: they already own admission, byte accounting, inflater progression, PIPE sequencing,
replies, barriers and terminal cleanup. The siblings are evidence for useful fields and failure
volume, not source to copy wholesale. In particular, `../Firmware`'s terminal PIPE NACK records
the expected/highest sequence, queued depth and negotiated window; those facts all exist in
`od_pipe.c` and remain useful after promotion.

This plan does not include the config remainder, RXQ throttling, Nordic newline hardware capture,
or cosmetic rewriting of target HAL/panel text.

## 2. Baseline and outcome

Stage 5 promoted progress, not the entire lifecycle. Shared code currently reports START mode,
first/final frame samples, 5% progress, compression ratio, throughput and completion. It also has
one PIPE origin-refusal warning. The remaining portable target text is small: ESP32 reports the
shared-transfer watchdog before full session teardown, and Nordic prints the generic
`dw init begin` from its panel adapter. Most of this follow-on is therefore new observability at
existing shared decisions, not another large deletion pass.

When complete, one capture must answer all of these without reading wire replies:

- why START was refused;
- why a live transfer stopped, and how far it got;
- whether failure was peer input, decompression, panel write, reply delivery, barrier or refresh;
- which PIPE window state caused a fatal DATA/END result; and
- whether PIPE saw reordering/retransmission, without logging every frame or SACK.

No command result, ACK/NACK byte, response order, etag rule, transfer timeout threshold, accepted
trailing-field behavior, raw full-frame truncation tolerance, panel callback order or cleanup
policy may change. A logging call must not become the only place a value is computed or a side
effect occurs.

## 3. Design decisions

### R-D1 — Log at the shared detector; add no report seam

`od_xfer.c`, `od_xfer_direct.c`, `od_xfer_partial.c` and `od_pipe.c` call `od_log_*` directly.
There is no `od_xfer_app_report()` and no target-formatted event callback. The target seam keeps
only panel operations, its clock and physical teardown policy.

Do not put a generic record in `od_xfer_abort_active()`. The abort reason is too coarse to say
whether `STREAM_FAILED` means malformed compressed input, an expected-size overflow or a panel
short write, and `REPLY_FAILED` is already diagnosed by `od_reply.c`. Each failing branch logs
before calling the cleanup helper. `od_xfer_abort_active()` remains cleanup-only.

### R-D2 — One throttle primitive and one transfer peer-warning budget

Known-command validation is peer-controlled and can arrive at BLE write rate. Add the pure,
header-only `shared/core/od_log_budget.h` primitive:

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t last_ms;
    bool armed;
} od_log_budget_t;

static inline bool od_log_budget_allows(od_log_budget_t *budget, uint32_t now_ms,
                                        uint32_t interval_ms)
{
    if (budget == NULL
        || (budget->armed && (uint32_t)(now_ms - budget->last_ms) < interval_ms)) {
        return false;
    }
    budget->last_ms = now_ms;
    budget->armed = true;
    return true;
}
```

Use unsigned subtraction for wrap and the explicit `armed` bit for uptime zero. R0 mechanically
migrates the equivalent private helpers in `od_session.c` and `od_dispatch.c`; their bucket counts,
five-second intervals and behavior do not change.

Transfer adds exactly **one** five-second budget shared by every peer-driven START/DATA/END WARN.
One malformed class may therefore suppress another during that interval. That is deliberate: the
wire result remains exact, the first diagnostic remains visible, and transfer logging does not add
another family of per-cause throttles. Local terminal ERROR records and DEBUG summaries are not
throttled because each is structurally limited to once per transfer episode.

Expose one private `od_xfer_peer_warning_allowed()` through `od_xfer_internal.h`, with its single
budget stored in `od_xfer.c`. It obtains `now_ms` from the existing `od_xfer_app_now_ms()` seam only
when WARN logging is compiled in; lower-level builds use a no-clock macro arm. Direct, partial and
PIPE record helpers all use this one gate rather than creating translation-unit-local budgets.

All budget state is compiled only at `OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_WARN`. `OD_CAP_LOG=0`
must retain neither state nor clock calls.

### R-D3 — Classify admission and stream failure without guessing from wire bytes

Add a private `od_xfer_start_cause_t` in `od_xfer_internal.h` covering malformed request,
unsupported flags, etag mismatch, unsupported partial mode, rectangle bounds, rectangle alignment,
panel geometry, declared-size mismatch and panel begin. It is a diagnostic classification, not a
wire enum. The PIPE arm helpers return the cause through an output parameter while preserving
their existing `OD_ERR_PIPE_START_*` output and ACK/NACK behavior. Direct and partial branches use
the same cause-to-text helper at their existing decision points.

Do not infer the diagnostic from a response byte. Several local causes deliberately share one
wire error (`OD_ERR_PARTIAL_STREAM` in particular), and full PIPE arm currently maps both missing
geometry and total-size mismatch to `OD_ERR_PIPE_START_SIZE_MISMATCH`. The private cause keeps the
log precise without changing those compatibility mappings.

Change internal `od_xfer_stream_push()` from `bool` to an internal consume result enum with at
least:

- `OD_XFER_STREAM_OK`;
- `OD_XFER_STREAM_INVALID` (bad scratch/arguments; internal error);
- `OD_XFER_STREAM_INFLATE_FAILED` (bad/truncated/oversize compressed stream); and
- `OD_XFER_STREAM_WRITE_FAILED` (the target sink refused produced bytes).

Reset a file-static sink-failed flag immediately before every pump call; `stream_sink()` sets it
only when `od_xfer_app_write()` short-writes or refuses bytes. The pump already rejects output
beyond the declared size before invoking the sink. This distinguishes peer stream failure from
panel write failure without changing `od_zlib_pump` or comparing its error strings. Do not print
`od_inflate_app_error()` text: backend wording differs between targets, and the portable category
plus byte counts is the stable diagnostic contract.

Return the same classification from `od_xfer_pipe_consume()` for raw and compressed input. Add a
size/refusal value if needed for raw partial overflow. `od_pipe.c` then chooses the unchanged wire
error byte and emits the one terminal record with the real local cause; it must not emit a generic
PIPE failure after `od_xfer.c` has already logged the same event. Direct and partial callers log
the classified result at their own existing terminal branch.

The result enum is private to `od_xfer_internal.h`; no target header or wire contract changes.

### R-D4 — Move timeout diagnosis into transfer ownership

Append `OD_XFER_ABORT_TIMEOUT` to `od_xfer_abort_reason_t` and add:

```c
bool od_xfer_abort_if_timed_out(uint32_t now_ms, uint32_t limit_ms);
```

The function returns false for no active transfer or a zero limit. It preserves the existing
strict `elapsed > limit` rule, logs before clearing state, aborts with
`OD_XFER_ABORT_TIMEOUT`, and returns true exactly once. Unsigned elapsed arithmetic preserves
uptime-wrap behavior.

ESP32 still schedules the check, captures transfer origin/link identity before the call, and runs
`abortToKnownState()` after a true result so crypto, queues and the owning link are handled in the
existing order. Its `od_xfer_app_abort()` force-off set gains `OD_XFER_ABORT_TIMEOUT`; otherwise
the new direct abort would accidentally weaken the old RESET-driven panel shutdown. The target's
`Shared transfer timeout - aborting session` line is deleted, while the separate `[abort] shared
transfer watchdog ...` session-teardown record remains target-owned. Nordic has no corresponding
timeout scheduler and needs no new caller.

Keep `od_xfer_started_ms()` for existing callers/tests; removing a harmless query is not part of
this change.

### R-D5 — Log-level and volume contract

| Level | Contract |
|---|---|
| ERROR | Internal invariant/resource failure, panel begin/write/refresh invocation failure, transfer watchdog, or a local terminal PIPE failure. Each is terminal or otherwise once per transfer. |
| WARN | Peer-controlled refusal, malformed/oversize stream input, owner mismatch, incomplete END, barrier abort, or physical refresh not completed. All peer-controlled cases share the one R-D2 budget. |
| INFO | An active transfer replaced by a new START. Existing bounded completion summary remains INFO. |
| DEBUG | PIPE negotiation and one terminal transport summary. No individual DATA, reorder, duplicate or SACK record. |

Use capitalized text without `ERROR:`/`WARNING:` prefixes. Use `%u`, `%d`, `%s` and bounded
32-bit values only; Nordic's newlib-nano path must not depend on `%llu` or floating-point
formatting. Compute helper results before `od_log_*` calls so the existing nested-call ratchet
continues to prove capability-off has no hidden side effects.

The mode mapping is fixed as `direct full`, `direct partial`, `PIPE full`, `PIPE partial` and
`fatal`. Admission causes use these fixed phrases: `malformed request`, `unsupported flags`,
`etag mismatch`, `partial update unsupported`, `rectangle out of bounds`, `rectangle not
byte-aligned`, `panel geometry unavailable`, `declared size mismatch`, and `panel preparation
failed`. Numeric flags, sizes and rectangles may be appended by the branch that already computed
them; do not compute them only for logging.

PIPE DATA cause phrases are likewise fixed: `frame exceeds negotiated size`, `reorder queue
full`, `sequence outside negotiated window`, `malformed compressed stream`, `size limit
exceeded`, `panel write failed`, and `invalid internal buffer`. The final `%s` in the PIPE state
suffix is either empty or `, partial`; it does not carry free-form backend text.

### R-D6 — Log state is private and capability-gated

Do not add logging fields to `od_xfer_state_t` or `od_pipe_state_t`. Keep them in file-static
blocks gated at the level that consumes them:

- `od_xfer.c`: the single WARN budget and any once-per-episode flags;
- `od_pipe.c` DEBUG block: SACK count, reordered-frame count, duplicate count and maximum queued
  depth.

Reset the PIPE debug counters when a PIPE START is accepted, not from an executable-local compile
definition. This keeps mixed-profile host archives ABI-safe and makes `OD_CAP_LOG=0` carry no
diagnostic RAM.

Counter meanings are fixed: `frames` is the existing count of unique frames consumed in order
(including frames later drained from the reorder queue); `SACKs` counts only responses
successfully queued; `reordered` counts unique ahead-of-window frames admitted to a slot;
`duplicates` counts already-queued or already-consumed frames discarded; and `max queued` is the
high-water depth of occupied reorder slots. Failed SACK attempts are already reported by
`od_reply.c` and do not masquerade as delivered acknowledgements.

### R-D7 — Preserve the target boundary

Only two target policy lines are removed:

- ESP32 `Shared transfer timeout - aborting session`, replaced by R-D4; and
- Nordic `dw init begin`, replaced by the existing shared START record plus the new outcomes.

Keep panel power, touch suspension, controller-plane switching, busy assertion/release timing,
SPI faults, FastEPD operations, physical refresh timing and BLE notification/subscription lines
target-owned. In particular, ESP32's `EPD refresh: ...`, its busy-pin diagnostics, Nordic's
`refresh: busy ...` detail and `pipe notifications ...` do not move. ESP32's generic
`Refresh timed out` WARN is the one wording adjustment: make it a DEBUG physical-cause record
(`Refresh busy remained asserted at timeout`) once shared owns the terminal WARN, so one timeout
does not produce two same-level outcome lines.

## 4. Event matrix

The text below is the contract to pin in exact-capture tests. A row may change before its owning
stage lands; after that, wording changes are ordinary reviewed behavior changes.

| Event and detector | Level | Record template | Suppression / duplication rule |
|---|---:|---|---|
| `od_xfer_replace_active()` sees active state | INFO | `Transfer replaced: mode=%s, written=%u/%u B` | Once per replacing START. Log before state clear. |
| `od_xfer_abort_if_timed_out()` expires | ERROR | `Transfer timed out after %u ms (mode=%s, written=%u/%u B)` | Terminal once. Target session-abort line may follow; it describes broader teardown. |
| Direct START malformed/declared-size mismatch | WARN | `Direct write START refused: %s` | Shared peer budget. |
| Partial START flags/etag/support/rectangle refusal | WARN | `Partial write START refused: %s` | Shared peer budget; reason strings map from the exact branch, not only the reused wire error byte. |
| PIPE START header/version/flags/frame/partial/size refusal | WARN | `PIPE START refused: %s (error=0x%02X)` | Shared peer budget. Keep response-before-activation ordering unchanged. |
| Panel info/geometry cannot admit START | ERROR | `%s START failed: panel geometry unavailable` | Once per command; not peer-throttled because it diagnoses local config/capability state. |
| Any target `begin_*` returns false | ERROR | `%s START failed during panel preparation` | Terminal once. `%s` is `Direct write`, `Partial write`, or `PIPE`. |
| DATA/END comes from the wrong owner | WARN | `Transfer frame refused: owner mismatch (opcode=0x%04X)` | Shared peer budget. Do not log each re-offered frame. |
| Compressed input is malformed, truncated or expands past the declared size | WARN | `Compressed transfer refused during %s (received=%u, written=%u/%u B)` | Shared peer budget, then terminal cleanup. Phase is `START`, `DATA`, or `END`. |
| Raw partial input or a byte counter exceeds its remaining/range limit | WARN | `Transfer DATA refused: size limit exceeded (received=%u, written=%u/%u B)` | Shared peer budget, then terminal cleanup. Do not apply this to the deliberately truncated raw full-frame tail. |
| Stream arguments/scratch violate an internal invariant | ERROR | `Transfer stream failed: invalid internal buffer` | Terminal once; no backend error string. |
| Target sink short-writes/refuses | ERROR | `Panel write failed at offset %u (%u B offered)` | Terminal once. Target SPI detail may precede it and is not duplicated in shared text. |
| Direct/partial END is incomplete | WARN | `Transfer END refused: incomplete stream (written=%u/%u B)` | Terminal once. |
| Refresh barrier returns ABORT | WARN | `Transfer refresh barrier aborted` | Once; no generic abort record. |
| `od_xfer_app_refresh()` returns false | ERROR | `Transfer refresh invocation failed` | Terminal once. |
| Refresh call succeeds with `completed=false` | WARN | `Transfer refresh did not complete` | Once. Delete/demote only a target line that is a byte-for-byte semantic duplicate; retain physical cause/timing detail. |
| PIPE START accepted | DEBUG | `PIPE started: %s, window=%u, ack every=%u, frame=%u B` | Once. Existing `DW start` remains the byte/compression summary. |
| `pipe_send_data_nack()` for a peer-caused frame/sequence/stream error | WARN | `PIPE DATA refused: %s (error=0x%02X, expected=%u, highest=%u, queued=%u, window=%u%s)` | Shared peer budget; fatal-state transition makes it once per episode. Cause comes from the frame/window branch or classified consume result. |
| `pipe_send_data_nack()` for an internal/panel failure | ERROR | `PIPE DATA failed: %s (error=0x%02X, expected=%u, highest=%u, queued=%u, window=%u%s)` | Terminal once by the fatal-state transition. Do not precede it with a second generic transfer record. |
| PIPE END with incomplete/reorder state | WARN | `PIPE END refused: incomplete stream (queued=%u, written=%u/%u B)` | Once; a previously logged fatal DATA error is not repeated at END. |
| PIPE stream reaches END/auto-END | DEBUG | `PIPE summary: frames=%u, SACKs=%u, reordered=%u, duplicates=%u, max queued=%u` | Exactly once on successful stream completion. No per-SACK or per-gap line. |

`od_reply.c` remains the sole owner of response queue/seal failures. Transfer cleanup may still use
`OD_XFER_ABORT_REPLY_FAILED`, but it emits no second line. A failed SACK send likewise relies on
the response diagnostic and the PIPE fatal state; it does not add `PIPE DATA failed` with a fake
wire error.

Tolerated behavior is not mislabeled as failure. A raw full-frame DATA tail that extends beyond
the remaining byte count stays silently truncated exactly as today, and trailing START/END fields
that the protocol accepts stay accepted without warning.

## 5. Implementation stages

Each stage is independently revertible. Because R0-R3 touch `shared/`, each runs the full
all-target gate before submission.

### R0 — One log-budget primitive

- Add `od_log_budget.h` as pure C99, header-only code.
- Replace the private budget structs/helpers in `od_session.c` and `od_dispatch.c` without changing
  intervals, bucket separation or call order.
- Extend the existing session/dispatch tests to prove uptime zero, suppression, exact five-second
  re-arm and unsigned wrap still behave identically.
- Add no transfer records yet.

### R1 — Lifecycle, timeout and stream classification

- Add `OD_XFER_ABORT_TIMEOUT`, `od_xfer_abort_if_timed_out()` and the replacement/timeout records.
- Convert `od_xfer_stream_push()` and `od_xfer_pipe_consume()` to the internal classified result
  and update every direct, partial and PIPE caller without changing its command result, wire error
  or cleanup reason.
- Update ESP32's timeout check to the shared API, add timeout to its force-off set, and delete only
  the superseded timeout line.
- Pin replacement, timeout, wrap, zero-limit, inflate-failure and sink-failure text/counts in
  `xfer_test.c` under INFO and DEBUG builds.

### R2 — Direct and partial START/DATA/END outcomes

- Add the private admission-cause enum/mapping and use it without changing wire error bytes.
- Add branch-specific admission records in `od_xfer_direct.c` and `od_xfer_partial.c`.
- Add owner, incomplete, barrier and refresh outcome records at their detectors.
- Remove Nordic's generic `dw init begin`; retain its panel/SPI/refresh details.
- Make ESP32's generic refresh-timeout WARN the DEBUG physical-cause record specified in R-D7.
- Prove reply failures still produce no transfer-owned duplicate.
- Add a fail-closed target-string ratchet for the two removed target policy lines and the one
  superseded generic refresh-timeout string.

### R3 — PIPE negotiation and terminal diagnostics

- Return the private admission cause from full/partial PIPE arm helpers while preserving their
  existing wire-error mapping.
- Add the accepted negotiation record and DEBUG-only private counters in `od_pipe.c`.
- Increment counters silently in `pipe_send_sack()` and the reorder/duplicate branches.
- Add the one cause-aware fatal DATA record before state mutation, incomplete END record, and
  successful terminal summary. Peer causes are WARN-budgeted; internal/panel causes are ERROR.
- Do not log SACK masks, individual gaps, individual retransmissions or post-fatal DATA frames.
- Keep BLE subscription/notification text in the target adapter.
- Mark the four L4 remainder boxes in the parent convergence plan's § 5 complete only after R3
  gates pass.

## 6. Verification plan

### Host exact-capture tests

Extend `xfer_test.c` with exact-level, exact-text and exact-count helpers. Cover at minimum:

- replacement with byte counts, including replacement of FATAL state;
- timeout below/equal/above the strict limit, uptime wrap, zero limit and a second call after
  state clear;
- timeout reaches `od_xfer_app_abort(OD_XFER_ABORT_TIMEOUT)` exactly once;
- each direct/partial START refusal family and the single shared WARN budget;
- budget suppression across two different causes, then re-arm at exactly 5000 ms;
- compressed START/DATA/END failure versus target sink failure;
- wrong owner, incomplete END, barrier abort, refresh invocation failure and
  `completed=false`; and
- reply failure has the existing reply record only, with no transfer duplicate.

Keep both `od_xfer_test` (DEBUG) and `od_xfer_info_test` (shipping INFO) over the same behavioral
suite. DEBUG-only expectations are conditional; WARN/ERROR/INFO text must match in both.

Extend `pipe_test.c` to capture records rather than linking `od_log_test_stub.c` blindly. Add a
DEBUG `od_pipe.c` archive or executable so executable-local `OD_LOG_LEVEL` does not pretend to
change an object already built in `od_shared`. Cover:

- requested-to-effective W/N/frame negotiation;
- one fatal record for oversize, inflater, sink and out-of-window failures, with exact cause and
  WARN/ERROR level;
- no second fatal record from subsequent DATA or END;
- one incomplete END record with queue and byte counts;
- reordered/duplicate/SACK counters and exactly one success summary;
- no per-frame/per-SACK DEBUG line during a full-window burst;
- the same behavior under the W=16 profile; and
- unchanged exact ACK/NACK bytes and response-before-activation ordering in every existing case.

### Capability-off and structural proofs

- Remove `od_log_test_stub.c` from `od_xfer_silabs_test` if the capability-off archive links
  without it, then prove the resulting binary has no `_od_log` symbol/reference.
- Extend `pipe_off_link_proof()` to resolve the capability-off `od_pipe.c.o` fail-closed and assert
  zero undefined `_od_log` references, alongside its existing state/entry-point checks.
- Resolve the BG22-profile `od_xfer*.c.o` files and assert zero `_od_log` references and no
  transfer/PIPE log-budget or debug-counter symbols.
- Use `strings` on the capability-off fixtures to prove the new `Transfer`/`PIPE` format literals
  are absent as well as unreachable.
- Keep all diagnostic fields out of `od_xfer_internal.h`; mixed INFO/DEBUG archives must have the
  same `od_xfer_state_t` layout.
- Extend `tools/check.sh` so the two retired target strings fail if restored, without banning
  target panel/refresh logging generally.
- The existing shared-boundary and nested-log-call ratchets remain mandatory.

### Repository gates

Run after every stage:

```text
ASAN_OPTIONS=detect_leaks=0 ./tools/check.sh --fuzz-time 1
ASAN_OPTIONS=detect_leaks=0 ./tools/check.sh --targets --fuzz-time 1
```

The first must report zero failures (target skips are expected without `--targets`); the second
must report zero failures and zero skips across every ESP32 fragment, all three Nordic boards and
BG22. Run `git diff --check` as well. Do not quote old host-test counts after adding fixtures;
report the count emitted by that run.

No dispatch corpus vector or protocol header should change. Replaying the pinned wire corpus and
the existing direct/partial/PIPE host suites is the proof that logging did not change wire
behavior.

### Hardware qualification

Hardware capture is recommended but not a merge gate because the records use the already-built
log transport and do not alter wire behavior. The useful post-merge captures are:

- ESP32: successful raw direct write, malformed compressed END, and forced transfer watchdog;
- Nordic: successful PIPE transfer with one induced reorder/retransmit and one incomplete END;
- both: confirm one terminal outcome, no per-DATA/SACK flood, and unchanged client-visible result.

If run, record the board, opcode, transport, encryption/compression mode and result in
`docs/HARDWARE_VERIFICATION_CHECKLIST.md`; do not put row-level evidence in this plan.

## 7. Definition of done

- [ ] R0-R3 landed independently with their exact-capture tests.
- [ ] The four unchecked L4 remainder boxes in the parent convergence plan's § 5 are marked
  complete.
- [ ] Every portable START/DATA/END/timeout/PIPE terminal decision in the matrix has one shared
  diagnostic owner.
- [ ] Peer-driven WARN traffic is bounded by one shared transfer budget; terminal records and
  PIPE summaries are structurally once per transfer.
- [ ] `od_reply`, target session teardown and target panel/transport logs are not duplicated.
- [ ] ESP32's timeout behavior, including owner selection, force-off and link teardown, is
  unchanged apart from which layer emits the timeout record.
- [ ] Direct, partial and PIPE ACK/NACK bytes and ordering are unchanged in host fixtures and the
  pinned corpus.
- [ ] BG22 retains no logging code, strings, state or `_od_log` linkage from the new paths.
- [ ] `tools/check.sh --targets` passes with zero failures and zero skips.
- [ ] The original plan status continues to distinguish complete Stages 0a-9 from this follow-on;
  Nordic CDC ACM/RTT newline capture remains a separate open qualification item.
