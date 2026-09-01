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

The primary outcome is **one comprehensive shared summary at the end of every admitted image
upload, whether it succeeds or fails**. Success widens the existing `DW complete` record rather
than adding another line. Failure uses the same summary family with the terminal cause. The
summary carries enough bounded state to diagnose the episode on its own: mode, result/cause,
received and written/expected bytes, chunk count and elapsed time, plus compression and PIPE
statistics when applicable. It is emitted exactly once before the last required state is lost.
Where existing ordering clears shared state before the final response is queued, capture these
fields in a stack-local terminal snapshot first and emit only after that response determines the
outcome. Do not widen `od_xfer_state_t` or retain a second persistent transfer state for logging.
This deliberately changes capture-visible log ordering: the current optimistic `DW complete`
appears after the first END ACK but before the barrier, refresh and final response; G0 appears only
after the terminal refresh/reply decision. Command ACK/NACK bytes and their ordering do not change,
but exact-capture tests must move the summary expectation to its new, truthful position.

A malformed or refused START that never admits a transfer is not an upload episode; it retains one
bounded refusal record instead of manufacturing a zero-byte terminal summary. Lower-layer reply,
panel or transport detail may precede the summary, but no second shared lifecycle outcome may
restate it.

With that summary present, one capture must answer all of these without reading wire replies:

- why START was refused;
- why a live transfer stopped, and how far it got;
- whether failure was peer input, decompression, panel write, reply delivery, barrier or refresh;
- which PIPE window state caused a fatal DATA/END result; and
- whether PIPE saw reordering/retransmission, without logging every frame or SACK.

No command result, ACK/NACK byte, response order, etag rule, transfer timeout threshold, accepted
trailing-field behavior, raw full-frame truncation tolerance, panel callback order or cleanup
policy may change. A logging call must not become the only place a value is computed or a side
effect occurs.

### 2.1 Ranked goals

These three goals outrank the event matrix. Where § 3 or § 4 conflicts with them, they win and the
conflicting row changes.

**G0 — End every admitted upload with one comprehensive summary.** The existing success
completion record becomes the successful form of this summary; it is not followed by a second
PIPE terminal record. Every terminal failure path supplies its typed cause to the same summary
family before cleanup discards byte, timing or PIPE state. Branch-local failure text may provide
lower-layer detail, but it must not become a second lifecycle outcome. PIPE negotiation remains a
separate DEBUG record because it describes the accepted transport contract, not the result.

One private, capability-gated `terminal_emitted` bit is the minimum state needed for paths such as
PIPE fatal and timeout, which report the outcome before a later reset clears the episode. A reset
emits `outcome=aborted` only when an active upload has not already emitted G0; replacement of an
already-summarized FATAL episode likewise clears silently. Reset the bit with transfer state. This
bit prevents duplicate summaries and does not enter `od_xfer_state_t` or any target-visible ABI.

Success is INFO. A refused or failed peer-controlled upload is WARN; an internal, target-resource
or watchdog failure is ERROR; replacement retains INFO as a normal lifecycle event while still
ending the displaced upload with `outcome=replaced`. Keep each fixed format within
`OD_LOG_TEXT_MAX` using only bounded 32-bit fields and newlib-nano-safe conversions.

**G1 — Simplify the code first; add a record second.** Every step must leave the transfer plane
structurally smaller or flatter than it found it. Concretely: prefer deleting a branch to
describing one, replace a private helper rather than adding a parallel one, and never split a
branch, widen a struct or introduce a seam whose only consumer is a log call. If a decision is
hard to describe in one record, that is evidence the decision is in the wrong place — move or
merge it rather than emitting two records to cover the seam. A stage that adds observability and
no simplification is acceptable only when no simplification was available, and the stage note must
say so.

Simplifications already identified as in scope, not optional extras:

- `od_log_budget.h` **replaces** the private budget structs in `od_session.c` and `od_dispatch.c`;
  R0 is a net deletion, not a third copy.
- The classified stream/consume result **replaces** the `bool` return; no caller keeps a parallel
  boolean or re-derives the cause from a wire byte.
- The private admission cause **replaces** open-coded refusal branches where two branches
  differ only by which error byte they send.
- `od_xfer_report_timeout()` moves the whole timeout decision into `od_xfer.c`, so
  `od_xfer_started_ms()` loses its only production caller. Delete it and the two `xfer_test.c`
  assertions that exist only to exercise it. A public query kept alive solely by its own test is
  exactly the residue G1 exists to remove — this supersedes R-D4's earlier note to keep it.

**G2 — Do not over-log.** The target is the fewest records that answer § 2's five questions, not
full coverage of every branch. A record earns its place only if a field capture would be
ambiguous without it. Specifically:

- One shared lifecycle outcome per decision, at one owner. If `od_reply.c`, a target panel log or
  an already-emitted shared record covers the lower-layer detail, add no second detail record; an
  admitted terminal episode still receives its one G0 summary.
- No record on a success path except the existing bounded START/progress diagnostics, the one PIPE
  negotiation record and the G0 terminal summary.
- Nothing per frame, per SACK, per gap, per retransmission or per queued slot.
- A terminal path emits exactly one shared lifecycle summary, from the layer that classified the
  outcome. Lower-layer physical or reply detail may precede it without restating the outcome.
- Tolerated behavior is silent: accepted trailing fields and the truncated raw full-frame tail
  produce no record at any level.
- When in doubt between a new record and a wider existing one, widen the existing one.

The matrix in § 4 is a ceiling, not a quota. A row that proves unnecessary once its neighbours
land should be dropped in review rather than implemented to satisfy the table.

## 3. Design decisions

### R-D1 — Log at the shared detector; add no report seam

`od_xfer.c`, `od_xfer_direct.c`, `od_xfer_partial.c` and `od_pipe.c` call `od_log_*` directly.
There is no `od_xfer_app_report()` and no target-formatted event callback. The target seam keeps
only panel operations, its clock and physical teardown policy.

Do not put an unclassified generic record in `od_xfer_abort_active()`. The abort reason is too
coarse to say whether `STREAM_FAILED` means malformed compressed input, an expected-size overflow
or a panel short write, and `REPLY_FAILED` detail is already diagnosed by `od_reply.c`. Where a
branch owns both the typed terminal cause and cleanup, replace the coarse cleanup call with one
terminal path that emits G0 and performs the same cleanup. The timeout detector is the exception:
it emits G0 and returns true so ESP32's existing `abortToKnownState()` ordering remains the cleanup
owner. No helper is introduced solely to print a line.

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

Transfer adds exactly **one** five-second budget shared by pre-admission and nonterminal
peer-driven START/DATA/END WARN traffic. One malformed class may therefore suppress another during
that interval. That is deliberate: the wire result remains exact, the first diagnostic remains
visible, and transfer logging does not add another family of per-cause throttles. A G0 terminal
summary is never budget-suppressed, including when its severity is WARN, because it is the sole
outcome for that admitted episode. Local terminal ERROR records and DEBUG diagnostics are likewise
not throttled because terminal state makes them structurally once per episode.

Expose one private `od_xfer_peer_warning_allowed()` through `od_xfer_internal.h`, with its single
budget stored in `od_xfer.c`. It obtains `now_ms` from the existing `od_xfer_app_now_ms()` seam only
when WARN logging is compiled in; lower-level builds use a no-clock macro arm. Direct, partial and
PIPE pre-admission/nonterminal record helpers all use this one gate rather than creating
translation-unit-local budgets. G0 never calls it.

All budget state is compiled only at `OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_WARN`. `OD_CAP_LOG=0`
must retain neither state nor clock calls.

### R-D3 — Classify admission and stream failure without guessing from wire bytes

Add a private `od_xfer_start_cause_t` in `od_xfer_internal.h` covering malformed request,
unsupported flags, etag mismatch, unsupported partial mode, rectangle bounds, rectangle alignment,
panel geometry, split-panel layout, zero declared size and declared-size mismatch. Panel begin is
not an admission cause: it occurs after state has been admitted and feeds G0 directly as
`panel preparation failed`. The PIPE arm helpers return the admission cause while preserving their
existing `OD_ERR_PIPE_START_*` output and ACK/NACK behavior. Direct and partial branches use the
same cause-to-text helper at their existing decision points.

Do not infer the diagnostic from a response byte. Several local causes deliberately share one
wire error (`OD_ERR_PARTIAL_STREAM` in particular), and full PIPE arm currently maps missing
geometry, split-panel layout, zero total and total-size mismatch into its existing refusal paths,
with the latter cases sharing `OD_ERR_PIPE_START_SIZE_MISMATCH`. The private cause keeps the log
precise without changing those compatibility mappings.

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

Add:

```c
bool od_xfer_report_timeout(uint32_t now_ms, uint32_t limit_ms);
```

The function returns false for no active transfer or a zero limit. It preserves the existing
strict `elapsed > limit` rule, emits the G0 timeout summary without clearing state, and returns
true. Its name deliberately advertises the reporting side effect: a caller that receives true
must immediately perform the existing teardown and must not treat it as a repeatable predicate.
Unsigned elapsed arithmetic preserves uptime-wrap behavior. The later reset sees
`terminal_emitted` and does not summarize the same episode again.

ESP32 still schedules the check, captures transfer origin/link identity before the call, and runs
`abortToKnownState()` after a true result so crypto, queues and the owning link are handled in the
existing order; the shared detector must not pre-clear panel or transfer state. The target's
`Shared transfer timeout - aborting session` line is deleted, while the separate `[abort] shared
transfer watchdog ...` session-teardown record remains target-owned. Nordic has no corresponding
timeout scheduler and needs no new caller.

`od_xfer_started_ms()` has no production caller once ESP32 stops scheduling its own comparison.
Delete it, its declaration in `od_xfer.h`, and the two `xfer_test.c` assertions that only exercise
it — see G1.

### R-D5 — Log-level and volume contract

| Level | Contract |
|---|---|
| ERROR | Internal invariant/resource failure, panel begin/write/refresh invocation failure, transfer watchdog, or a local terminal PIPE failure. Each is terminal or otherwise once per transfer. |
| WARN | Peer-controlled refusal, malformed/oversize stream input, owner mismatch, incomplete END, barrier abort, or physical refresh not completed. Pre-admission/nonterminal cases share the R-D2 budget; a G0 terminal summary never does. |
| INFO | Successful G0 summary, or an active transfer ended as `replaced` by a new START. |
| DEBUG | Existing bounded progress and PIPE negotiation. PIPE terminal statistics fold into G0; there is no second terminal transport summary. No individual DATA, reorder, duplicate or SACK record. |

Use capitalized text without `ERROR:`/`WARNING:` prefixes. Use `%u`, `%d`, `%s`, and fixed-width
uppercase `0x%02X`/`0x%04X` for bounded integral fields only; Nordic's newlib-nano path must not
depend on `%llu` or floating-point formatting. Compute helper results before `od_log_*` calls so
the existing nested-call ratchet continues to prove capability-off has no hidden side effects.

The mode mapping is fixed as `direct full`, `direct partial`, `PIPE full`, `PIPE partial` and
`fatal`. Admission/refusal causes use these fixed phrases: `malformed request`, `unsupported
flags`, `etag mismatch`, `partial update unsupported`, `rectangle out of bounds`, `rectangle not
byte-aligned`, `zero declared size`, and `declared size mismatch`. Local pre-admission failures use
`panel geometry unavailable` or `split-panel layout unsupported`. The admitted terminal cause
`panel preparation failed` appears only in G0. Numeric flags, sizes and rectangles may be appended
by the branch that already computed them; do not compute them only for logging.

PIPE DATA cause phrases are likewise fixed: `frame exceeds negotiated size`, `reorder queue
full`, `sequence outside negotiated window`, `malformed compressed stream`, `size limit
exceeded`, `panel write failed`, and `invalid internal buffer`. The final `%s` in the PIPE state
suffix is either empty or `, partial`; it does not carry free-form backend text.

Every fixed G0 format has a 232-byte proof obligation. At maximum 32-bit timestamp and cycle
widths, `_od_log()`'s prefix consumes 29 bytes, leaving at most 203 bytes for the summary text under
`OD_LOG_TEXT_MAX`. The longest permitted forms are deliberately compact:

```text
DW complete: mode=PIPE partial rx=4294967295 written=4294967295/4294967295 chunks=4294967295 elapsed=4294967295 ms rate=429496729.5 KB/s zlib=42949672.95x p[f=65535 a=65535 r=65535 d=65535 q=33]
DW failed: cause=sequence outside negotiated window mode=PIPE partial rx=4294967295 written=4294967295/4294967295 chunks=4294967295 elapsed=4294967295 ms error=0xFF zlib p[e=255 h=255 q=33 w=32]
```

They are 194 bytes each, leaving nine bytes of message headroom. `DW complete` already states the
successful outcome, so success does not spend another 18 bytes on `outcome=complete`. The PIPE
success legend is fixed as frames/SACKs/reordered/duplicates/max-queued; the failure legend is
expected/highest/queued/window. Raw, direct and non-PIPE forms omit only inapplicable suffixes and
are therefore shorter. Other failure-detail variants, including panel offset/offered bytes, must
prove they fit the same 203-byte ceiling.

The width contract is explicit: core byte/chunk/elapsed fields are `uint32_t` (10 digits); mode is
at most `direct partial` (14 characters), while the longest PIPE mode that carries the `p[...]`
suffix is `PIPE partial` (12 characters); the longest cause is
`sequence outside negotiated window` (34 characters); fixed-point rate is a `uint32_t` tenths
value (9 integer digits plus one decimal);
the zlib ratio is a `uint32_t` hundredths value (8 integer digits plus two decimals); wire error,
expected and highest are `uint8_t`; window is at most `OD_PIPE_MAX_W` (32); queued/max-queued are at
most `OD_PIPE_REORDER_SLOTS` (33); and the four cumulative PIPE success counts print as saturated
`uint16_t` values (5 digits). A printed cumulative value of 65535 means “65535 or more”; do not
narrow `s_pipe.received_count`, which remains protocol state.

Host fixtures set each field to its documented printable bound, then assert the summary portion is
at most 203 bytes, the complete captured record retains its final named field and CRLF, and the
text before CRLF is at most 232 bytes. Exercise `direct partial` as the 14-character global mode
maximum and `PIPE partial` as the 12-character suffix-bearing maximum. The PIPE suffix fixture also
asserts its formatter reports a fit in the supplied buffer. A format change that can silently
truncate fails this test; relying on `od_log.c`'s unmarked truncation is not acceptable.

### R-D6 — Log state is private and capability-gated

Do not add logging fields to `od_xfer_state_t` or `od_pipe_state_t`. Keep them in file-static
blocks gated at the level that consumes them:

- `od_xfer.c`: the single WARN budget and any once-per-episode flags;
- `od_pipe.c` INFO block: SACK count, reordered-frame count, duplicate count and maximum queued
  depth.

Reset the PIPE summary counters when a PIPE START is accepted, not from an executable-local compile
definition. This keeps mixed-profile host archives ABI-safe and makes `OD_CAP_LOG=0` carry no
diagnostic RAM.

The G0 `terminal_emitted` bit and core counters needed by a failure summary compile at the lowest
severity that can emit that summary and remain private to `od_xfer.c`. The INFO success counters
remain private to `od_pipe.c`. All of this diagnostic state disappears under `OD_CAP_LOG=0`.

G0 explicitly authorizes one exception to G1's no-log-only-seam rule:

```c
size_t od_pipe_log_suffix(char *buf, size_t size);
```

It is private to `od_xfer_internal.h` and its only caller is the G0 snapshot builder in
`od_xfer.c`. It formats the applicable PIPE window fields and, at INFO or DEBUG, the success
counters into the caller's bounded buffer before PIPE state is cleared. It returns zero and writes
an empty string when PIPE is inactive or logging capability is off. Keeping this as one bounded
formatter localizes both the G0 exception and the 232-byte record budget; do not add individual
counter accessors or widen either state struct.

Counter meanings are fixed: `frames` is the existing count of unique frames consumed in order
(including frames later drained from the reorder queue); `SACKs` counts only responses
successfully queued; `reordered` counts unique ahead-of-window frames admitted to a slot;
`duplicates` counts already-queued or already-consumed frames discarded; and `max queued` is the
high-water depth of occupied reorder slots. Format `frames` as a `uint16_t`-saturated view of the
existing protocol counter; store SACKs/reordered/duplicates as saturating `uint16_t` diagnostics
and max queued as a bounded `uint8_t`. Failed SACK attempts are already reported by `od_reply.c`
and do not masquerade as delivered acknowledgements.

### R-D7 — Preserve the target boundary

Only two target policy lines are removed:

- ESP32 `Shared transfer timeout - aborting session`, replaced by R-D4; and
- Nordic `dw init begin`, replaced by the existing shared START record plus the new outcomes.

Keep panel power, touch suspension, controller-plane switching, busy assertion/release timing,
SPI faults, FastEPD operations, physical refresh timing and BLE notification/subscription lines
target-owned. In particular, ESP32's `EPD refresh: ...`, its busy-pin diagnostics, Nordic's
`refresh: busy ...` detail and `pipe notifications ...` do not move. ESP32's generic
`Refresh timed out` remains WARN because `waitforrefresh()` also serves boot and other
non-transfer paths where no G0 summary exists. Its wording may become the more precise target-owned
physical detail `Refresh busy remained asserted at timeout`, but its visibility must not be
demoted. During an admitted upload that physical detail may precede the one G0 lifecycle summary;
it does not replace or duplicate the summary's transfer outcome.

## 4. Event matrix

The text below is the contract to pin in exact-capture tests. G0 is the terminal contract: every
row that ends an admitted upload contributes its cause and fields to the one comprehensive
success/failure summary rather than emitting an additional branch-local outcome. Pre-admission
refusals and nonterminal diagnostics remain separate bounded records. A row may change before its
owning stage lands; after that, wording changes are ordinary reviewed behavior changes.

| Event and detector | Level | Record template | Suppression / duplication rule |
|---|---:|---|---|
| Admitted upload completes successfully | INFO | `DW complete` G0 summary with mode, byte/chunk/time, compression and applicable PIPE fields | Exactly once, after the final success decision and before its snapshot is discarded. `complete` in the fixed prefix is the outcome; no `outcome=complete` field is added. |
| `od_xfer_replace_active()` sees active state | INFO | G0 summary with `outcome=replaced` | Once for the displaced upload, before state clear. |
| `od_xfer_reset()` clears an active upload during broader teardown | INFO | G0 summary with `outcome=aborted, cause=reset` | Emit only if G0 has not already reported this episode. This includes owner disconnect, deep sleep, idle timeout and auth-abuse teardown through `od_core_reset()`; their target teardown records own the more specific reason. No summary when transfer state is idle or already summarized. |
| Transfer timeout expires | ERROR | G0 failure summary with `cause=timeout` | Terminal once. Target session-abort detail may follow; it describes broader teardown. |
| Direct START malformed/declared-size mismatch | WARN | `Direct write START refused: %s` | Shared peer budget. |
| Partial START flags/etag/support/rectangle refusal | WARN | `Partial write START refused: %s` | Shared peer budget; reason strings map from the exact branch, not only the reused wire error byte. |
| PIPE START header/version/flags/frame/partial/zero-size/declared-size refusal | WARN | `PIPE START refused: %s (error=0x%02X)` | Shared peer budget. Zero size is named separately from a nonzero declared-size mismatch. Keep response-before-activation ordering unchanged. |
| Panel info/geometry/layout cannot admit START | ERROR | `%s START failed: %s` with `panel geometry unavailable` or `split-panel layout unsupported` | One pre-admission record; not peer-throttled because it diagnoses local config/capability state. It does not also use the generic admission wording. |
| Any target `begin_*` returns false after admission | ERROR | G0 failure summary with `cause=panel preparation failed` | Terminal once. Mode distinguishes direct, partial and PIPE. |
| DATA/END comes from the wrong owner | WARN | `Transfer frame refused: owner mismatch (opcode=0x%04X)` | Shared peer budget. Do not log each re-offered frame. |
| Compressed input is malformed, truncated or expands past the declared size | WARN | G0 failure summary with `cause=malformed compressed stream` and phase | Terminal once without the peer budget. Phase is `START`, `DATA`, or `END`. |
| Raw partial input or a byte counter exceeds its remaining/range limit | WARN | G0 failure summary with `cause=size limit exceeded` | Terminal once without the peer budget. Do not apply this to the deliberately truncated raw full-frame tail. |
| Stream arguments/scratch violate an internal invariant | ERROR | G0 failure summary with `cause=invalid internal buffer` | Terminal once; no backend error string. |
| Target sink short-writes/refuses | ERROR | G0 failure summary with `cause=panel write failed` and offset/offered bytes | Terminal once. Target SPI detail may precede it and is not duplicated in the summary. |
| Direct/partial END is incomplete | WARN | G0 failure summary with `cause=incomplete stream` | Terminal once. |
| Refresh barrier returns ABORT | WARN | G0 failure summary with `cause=refresh barrier aborted` | Terminal once; no generic abort record. |
| `od_xfer_app_refresh()` returns false | ERROR | G0 failure summary with `cause=refresh invocation failed` | Terminal once. |
| Refresh call succeeds with `completed=false` | WARN | G0 failure summary with `cause=refresh did not complete` | Terminal once. Retain target physical cause/timing detail without adding another lifecycle outcome. |
| PIPE START accepted | DEBUG | `PIPE started: %s, window=%u, ack every=%u, frame=%u B` | Once. Existing `DW start` remains the byte/compression summary. |
| `pipe_send_data_nack()` for a peer-caused frame/sequence/stream error | WARN | G0 failure summary with cause, wire error and PIPE window fields | Never budget-suppressed; the fatal-state transition makes it once per admitted episode. Cause comes from the frame/window branch or classified consume result. |
| `pipe_send_data_nack()` for an internal/panel failure | ERROR | G0 failure summary with cause, wire error and PIPE window fields | Terminal once by the fatal-state transition. Do not precede it with a second generic transfer outcome. |
| PIPE END with incomplete/reorder state | WARN | G0 failure summary with `cause=incomplete stream` and queued/window fields | Once; a previously summarized fatal DATA error is not repeated at END. |
| PIPE stream reaches END/auto-END | INFO | Fold `frames`, `SACKs`, `reordered`, `duplicates` and `max queued` into the G0 success summary | No separate PIPE terminal line and no per-SACK or per-gap line. |

`od_reply.c` remains the sole owner of response queue/seal failures. Transfer cleanup may still use
`OD_XFER_ABORT_REPLY_FAILED`, but it emits no second line. A failed SACK send likewise relies on
the response diagnostic and the PIPE fatal state; it does not add `PIPE DATA failed` with a fake
wire error.

Tolerated behavior is not mislabeled as failure. A raw full-frame DATA tail that extends beyond
the remaining byte count stays silently truncated exactly as today, and trailing START/END fields
that the protocol accepts stay accepted without warning.

## 5. Implementation stages

Each stage is independently revertible. Because R0-R3 touch `shared/`, each runs the full
all-target gate before submission. Each stage also names, in its commit message, the code it
removed or merged (G1) and any matrix row it decided not to implement (G2).

### R0 — One log-budget primitive

- Add `od_log_budget.h` as pure C99, header-only code.
- Replace the private budget structs/helpers in `od_session.c` and `od_dispatch.c` without changing
  intervals, bucket separation or call order.
- Extend the existing session/dispatch tests to prove uptime zero, suppression, exact five-second
  re-arm and unsigned wrap still behave identically.
- Add no transfer records yet.

### R1 — Lifecycle, timeout and stream classification

- Add `od_xfer_report_timeout()` and the replacement/timeout G0 outcomes without moving cleanup
  ahead of ESP32's existing teardown sequence.
- Convert `od_xfer_stream_push()` and `od_xfer_pipe_consume()` to the internal classified result
  and update every direct, partial and PIPE caller without changing its command result, wire error
  or cleanup reason.
- Update ESP32's timeout check to the shared API and delete both the superseded timeout line and
  the now-unused `od_xfer_started_ms()` query.
- Replace the existing completion record with the G0 success/failure summary family and pin
  replacement, timeout, wrap, zero-limit, inflate-failure and sink-failure text/counts in
  `xfer_test.c` under INFO and DEBUG builds.

### R2 — Direct and partial START/DATA/END outcomes

- Add the private admission-cause enum/mapping and use it without changing wire error bytes.
- Add branch-specific admission records in `od_xfer_direct.c` and `od_xfer_partial.c`.
- Add the nonterminal owner record and feed incomplete, barrier and refresh outcomes into G0 at
  their detectors.
- Remove Nordic's generic `dw init begin`; retain its panel/SPI/refresh details.
- Make ESP32's generic refresh-timeout text the WARN physical-cause record specified in R-D7,
  preserving boot/non-transfer visibility.
- Prove reply failures still produce no transfer-owned duplicate.
- Add a fail-closed target-string ratchet for the two removed target policy lines and the one
  superseded generic refresh-timeout string.

### R3 — PIPE negotiation and terminal diagnostics

- Return the private admission cause from full/partial PIPE arm helpers while preserving their
  existing wire-error mapping.
- Add the accepted DEBUG negotiation record and INFO-gated private summary counters in
  `od_pipe.c`.
- Add the one bounded `od_pipe_log_suffix()` formatter authorized by G0; do not expose individual
  PIPE counter accessors.
- Increment the `uint16_t` summary counters saturatingly in `pipe_send_sack()` and the
  reorder/duplicate branches; never wrap a diagnostic count back to zero.
- Feed the cause-aware fatal DATA and incomplete END outcomes into the G0 failure summary before
  state mutation. Fold the INFO PIPE counters into the G0 success summary instead of adding a
  second terminal record. Peer causes are WARN; internal/panel causes are ERROR.
- Do not log SACK masks, individual gaps, individual retransmissions or post-fatal DATA frames.
- Keep BLE subscription/notification text in the target adapter.
- Mark the four L4 remainder boxes in the parent convergence plan's § 5 complete only after R3
  gates pass.

## 6. Verification plan

### Host exact-capture tests

Extend `xfer_test.c` with exact-level, exact-text and exact-count helpers. Cover at minimum:

- exactly one comprehensive G0 summary for every admitted success and terminal failure, and no G0
  summary for a pre-admission START refusal;
- live replacement emits one G0 summary with byte counts; replacement of an already-summarized
  FATAL state emits no second summary;
- active reset emits one aborted G0 summary while an idle reset emits none;
- timeout below/equal/above the strict limit, uptime wrap, zero limit and a second call after
  state clear;
- `od_xfer_report_timeout()` itself does not abort or clear state, and a true result is followed
  immediately by the ESP32 caller's one existing teardown path;
- each direct/partial START refusal family and the single shared WARN budget;
- zero declared size and split-panel layout produce their distinct exact causes while preserving
  the existing wire error;
- budget suppression across two different causes, then re-arm at exactly 5000 ms;
- compressed START/DATA/END failure versus target sink failure;
- wrong owner, incomplete END, barrier abort, refresh invocation failure and
  `completed=false`; and
- reply failure retains its lower-layer reply detail and adds exactly one G0 failure summary, with
  no second transfer outcome.

Also pin the deliberate ordering change: no success summary exists after the first END ACK or
before barrier/refresh; it appears only after the final response outcome. Exercise every G0 fixed
format with maximum-width timestamp/cycle and each field's documented bound—including saturated
`uint16_t` PIPE counts—assert the final field and CRLF survive capture, and assert the text before
CRLF is at most the 232-byte `OD_LOG_TEXT_MAX` contract.

Keep both `od_xfer_test` (DEBUG) and `od_xfer_info_test` (shipping INFO) over the same behavioral
suite. DEBUG-only expectations are conditional; WARN/ERROR/INFO text must match in both.

Extend `pipe_test.c` to capture records rather than linking `od_log_test_stub.c` blindly. Add
separate INFO and DEBUG `od_pipe.c` archives or executables so an executable-local `OD_LOG_LEVEL`
does not pretend to change an object already built in `od_shared`. Cover:

- requested-to-effective W/N/frame negotiation;
- one G0 failure summary for oversize, inflater, sink and out-of-window failures, with exact cause
  and WARN/ERROR level;
- no second G0 summary from subsequent DATA, END, replacement or reset after a fatal outcome;
- one incomplete END G0 summary with queue and byte counts;
- reordered/duplicate/SACK counters folded into exactly one G0 success summary in both INFO and
  DEBUG builds;
- each cumulative counter stops at 65535 rather than wrapping, and max queued never exceeds
  `OD_PIPE_REORDER_SLOTS`;
- the all-maximum PIPE success/failure suffix fits, reports no truncation and remains present at
  the tail of the captured 232-byte-bounded record;
- no per-frame/per-SACK DEBUG line during a full-window burst;
- the same behavior under the W=16 profile; and
- unchanged exact ACK/NACK bytes and response-before-activation ordering in every existing case.

### Capability-off and structural proofs

- Remove `od_log_test_stub.c` from `od_xfer_silabs_test` if the capability-off archive links
  without it, then prove the resulting binary has no `_od_log` symbol/reference.
- Extend `pipe_off_link_proof()` to resolve the capability-off `od_pipe.c.o` fail-closed and assert
  zero undefined `_od_log` references, alongside its existing state/entry-point checks. Its
  capability-off `od_pipe_log_suffix()` arm must return zero and an empty string.
- Resolve the BG22-profile `od_xfer*.c.o` files and assert zero `_od_log` references and no
  transfer/PIPE log-budget, terminal-summary or summary-counter state.
- Use `strings` on the capability-off fixtures to prove the new `Transfer`/`PIPE` format literals
  are absent as well as unreachable.
- Keep all diagnostic state and struct fields out of `od_xfer_internal.h`; declarations such as
  the authorized `od_pipe_log_suffix()` are permitted. Mixed INFO/DEBUG archives must retain the
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
- [ ] G0 satisfied: every admitted upload emits exactly one comprehensive terminal summary on
  success, failure, replacement or active reset; pre-admission refusals emit none; PIPE contributes
  fields rather than a second terminal record.
- [ ] G1 satisfied: each stage deleted or merged more than it duplicated, no struct widened for a
  log, the sole log-only seam is G0's bounded `od_pipe_log_suffix()`, and
  `od_xfer_started_ms()` and the two private budget helpers are gone.
- [ ] G2 satisfied: every record in the merged set is justified by a § 2 question, no success path
  outside the named summaries logs, and no per-frame/per-SACK/per-gap record exists at any level.
- [ ] The four unchecked L4 remainder boxes in the parent convergence plan's § 5 are marked
  complete.
- [ ] Every portable START/DATA/END/timeout/PIPE terminal decision in the matrix has one shared
  diagnostic owner.
- [ ] Pre-admission/nonterminal peer WARN traffic is bounded by one shared transfer budget; the G0
  summary bypasses that budget and is structurally once per admitted transfer.
- [ ] INFO builds retain the PIPE summary counters, and maximum-width direct/partial/PIPE G0
  records prove their final field survives within the 232-byte text ceiling.
- [ ] The success summary's move from before refresh to after the final response outcome is pinned
  as a deliberate capture-ordering change; wire response order remains unchanged.
- [ ] `od_reply`, target session teardown and target panel/transport logs are not duplicated.
- [ ] ESP32's timeout behavior, including owner selection, force-off and link teardown, is
  unchanged apart from which layer emits the timeout record.
- [ ] Direct, partial and PIPE ACK/NACK bytes and ordering are unchanged in host fixtures and the
  pinned corpus.
- [ ] BG22 retains no logging code, strings, state or `_od_log` linkage from the new paths.
- [ ] `tools/check.sh --targets` passes with zero failures and zero skips.
- [ ] The original plan status continues to distinguish complete Stages 0a-9 from this follow-on;
  Nordic CDC ACM/RTT newline capture remains a separate open qualification item.
