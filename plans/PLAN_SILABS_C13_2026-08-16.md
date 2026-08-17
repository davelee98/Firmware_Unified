# Plan: C13 — `efr32bg22-slc` joins the shared command path

**Status:** NOT STARTED. Written 2026-08-16, after C12's software half landed. C12's hardware rows
(H1-H3) remain ACCEPTED-UNRUN and do **not** block this plan.

**REVISED 2026-08-17 after two reviews.** The second found a **critical defect in this plan's
central safety argument**: § 2.5 justified the 2,048-byte cap by saying `od_config_asm` already
refuses an oversize declaration, and it does not — it bounds against the 4,000-byte transferable
ceiling only (`shared/core/od_config_asm.c:63`), so a 2,048-byte buffer would accept a declared
total of 2,049..4,000 and overflow at `:110`. That is a remotely triggerable overflow on the 32 KB
part, reachable from a 2-byte length field. Fixing it is a shared **logic** change, which this plan
previously said it would not make. See § 2.5.

Seven further corrections land in the same revision: `!s_notify` must not map to RETRY, "no TX
backlog" is not achievable, the § 4.5 busy-loop argument was wrong, `od_core_reset()` is the wrong
D2 primitive, the GATT characteristic caps values at 244 rather than 253, the arena assert must
track `od_dispatch_budget()`, and error `0x07` does not mean "unsupported" in the PIPE namespace.

**Reviewed 2026-08-17 against the source**, and corrected in seven places. What the review
CONFIRMED is worth stating, because it is the load-bearing half: § 2.2's event-retention mechanism
is real (`sl_bt_step()` peeks and skips the pop; the SDK's own comment says the event "will be kept
in the stack's queue"), there are exactly two `OD_FRAME_DEFERRED` returns and § 2.2 enumerates both,
D8's arithmetic is exact to the byte (`6 + 218 = 224 = OD_SESSION_PLAIN_FRAME_MAX`), 3 slots is the
right depth, and every spot-checked Silabs line reference lands on the claimed code. Corrected: the
hook count (17 -> **21**), the watchdog premise (§ 4.1 — the part HAS one), the size of the config
storage change (§ 2.5 — it is an NVM3 record layout change), a self-contradiction about how many
shared edits this plan makes (§ 2.5 — one, not two), the missing depth assertion (§ 2.3), the
chunk arithmetic (§ 2.4), and the unrecorded `SL_CATALOG_KERNEL_PRESENT` precondition (§ 2.2).

**Parent:** [`PLAN_OD_DISPATCH_C12_2026-08-16.md`](PLAN_OD_DISPATCH_C12_2026-08-16.md) § 2.5 names
Silabs as C13. This document is that plan.

**Baseline:** `efr32bg22-slc` consumes `OD_SHARED_SOURCES_PURE` only
(`targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:270-285`), calls `od_advert`, and
open-codes everything else: a 529-line config parser, a 640 B `EncryptionSession` with hand-rolled
RFC 3610 CCM, and a full opcode switch inside a 1,303-line pipe file
(`targets/efr32bg22-slc/opendisplay_pipe.c:1089-1186`). It has **never been flashed from this
repo**; it builds headless (`./build-and-flash.sh --no-flash`).

---

## 1. Outcome

BG22 adopts the shared command path — dispatch, gate, egress, session and config — and closes eight
recorded defects, five of them security- or availability-relevant. The third copy of the config
parser and the second copy of hand-rolled CCM are deleted.

**It does this while reducing the target's static RAM**, which is the fact that makes the plan
viable at all: net **−236 B static and −544 B stack** (§ 4.6). The shared egress ring is not an
addition — it replaces more staging than it costs.

Four owner decisions, taken 2026-08-16, shape the scope and are settled inputs:

1. **No PIPE write.** 0x0080-0x0082 are not implemented and will not be.
2. **No RX queue, and no UNBOUNDED TX backlog.** Egress is a 3-slot arena drained in the same
   superloop pass. It is *bounded*, not *empty-at-end-of-pass*: `od_txq_process()` stops at the
   head on `OD_RADIO_RETRY` and keeps the entry, because ordering forbids skipping past it
   (`shared/core/od_txq.c:187-189`). A retained entry across passes is the RETRY design working,
   not a violation. What is excluded is a queue that can grow.
3. **No reorder, no target-owned resume.**
4. **`MAX_CONFIG_SIZE` stays 2048 on BG22**, deliberately re-opening the fleet-wide-4096 decision
   for this target.

Two shared changes, both in § 2.5 and both forced by the cap reversal: `OD_CONFIG_MAX_SIZE` gains
an `#ifndef` guard, and `od_config_asm_start()` gains the bound that makes the guard safe. No other
shared source changes.

---

## 2. The four decisions

### 2.1 No PIPE — free, and already the shared design

`OD_PIPE_ENABLE=0` must `#if` the reorder array out of *existence*, not merely skip the handlers: a
linked-but-unused 8.3 KB array is an instant link failure on a 32 KB part
(`docs/MEMORY_CONSTRAINTS.md:44-48`). Shared dispatch routes 0x0080-0x0082 to
`od_cmd_app_pipe_{start,data,end}`, and BG22 defines all three.

**One wire change, deliberate.** Today 0x0080 falls into `default:` and is silently dropped.
`DIVERGENCE_MATRIX:195` records that as wrong: a START should be *refused* so a client learns the
capability is absent instead of timing out. BG22 already sets that precedent itself, answering 0x76
and 0x77 with `{FF,cmd_lo,07,00}` fail-fast NACKs (`opendisplay_pipe.c:1177-1186`) — which the
matrix calls "correct behaviour for an unsupporting target — adopt as the default for any
compiled-out subsystem." **0x07 CANNOT BE REUSED, and the reason is a trap the canonical header flags itself.** BG22's
existing 0x76/0x77 answers are correct because in the *partial* namespace 0x07 is
`OD_ERR_PARTIAL_UNSUPPORTED` (`opendisplay_protocol.h:570`). In the **PIPE** namespace the same
byte is `OD_ERR_PIPE_START_RECT_INVALID` (`:612`), and the header warns in terms that
"0x03/0x05/0x06/0x07 mean different things in the two namespaces" (`:614`). A host reading
`{FF,80,07,00}` is told its rectangle was invalid — a per-request failure it may retry with
different geometry — not that the capability is absent. `py-opendisplay` treats it exactly that
way and caches no capability, so the sentence "a client learns the capability is absent" would be
false as implemented.

C13 therefore has three options and must pick one in C13.0 rather than at the keyboard:
  (a) answer `{FF,80,<code>,00}` with a code that is *not* meaningful in the PIPE namespace and
      document it as this target's "unsupported" marker;
  (b) keep 0x0080 silent, matching 0x0081/0x0082, and close the gap host-side; or
  (c) leave it and file the missing "unsupported PIPE" code as the protocol defect it is.
The canonical header is frozen, so inventing a code is out. **Record the choice and its host
consequence in FOLLOWUPS either way** — the current text claims a client outcome the wire does not
deliver. `0x0081`/`0x0082` return `OD_CMD_UNKNOWN` and stay silent — a NACK to a DATA frame is fatal to
an upload loop, and there is no upload here to protect.

### 2.2 No RX queue — and the BGAPI event queue is what makes `OD_FRAME_DEFERRED` safe

`od_rxq` is **not** linked. 34 slots × 264 B ≈ 8.9 KB is 28 % of the chip for a decoupling BG22 does
not need: `sl_bt_step()` pops one BGAPI event per superloop pass and calls `sl_bt_on_event()` inline
(`autogen/sl_event_handler.c:82`, `autogen/sl_bluetooth.c:69-90`), so arrival context *is* consumer
context.

**But declining it raises a real question, and the answer is the linchpin of this plan.** Shared
dispatch can return `OD_FRAME_DEFERRED` — the one outcome that does **not** consume the frame, which
must then be re-offered unchanged. On ESP32 and Nordic the RX ring holds it. BG22 has no ring, so a
DEFERRED frame would have nowhere to live and would be silently lost. There are exactly two sources:

- **producer conflict** — a CONFIG_READ is live and the new frame reads or mutates config
  (`shared/core/od_dispatch.c:183-191`);
- **reservation failure** — the egress arena has no capacity (`:194-199`).

`sl_bt_can_process_event()` (`autogen/sl_bluetooth.c:63-67`) answers both. It is weak, defaults to
`return true`, and returning false makes the stack **retain the event in its own queue** rather than
deliver or drop it. BG22 overrides it as:

```c
bool sl_bt_can_process_event(uint32_t len)
{
    (void)len;
    return !od_config_read_active() && od_txq_depth() == 0u;
}
```

**THE HOOK IS ALL-OR-NOTHING, and that is what makes the radio HAL's `!s_notify` mapping load
bearing.** It sees only the next event's length, so it cannot admit a CCC-status or connection-closed
event while holding a GATT write. Combined with a `!s_notify -> RETRY` mapping that would deadlock
permanently: a client that writes before subscribing gets a response queued, the send fails for want
of a subscription, the entry is retained, the queue is non-empty, and the CCC-enable event that
would set `s_notify` is held forever. § 4.2 resolves it on the HAL side — `!s_notify` is **not**
RETRY. The same reasoning applies to any state this hook can starve: if the only thing that can
clear a RETRY is an application event, the pair deadlocks.

**One precondition, and it has no build-time signal.** `sl_bt_can_process_event()` and `sl_bt_step()`
both sit inside `#if !defined(SL_CATALOG_KERNEL_PRESENT)` (`autogen/sl_bluetooth.c`): under an RTOS
"the stack events are processed in a dedicated event processing task, and these functions are not
used at all". BG22 is a superloop with no kernel (CLAUDE.md decision 6) and
`autogen/sl_component_catalog.h` defines no such symbol today — verified — so the override is live.
But adding a kernel component later would leave the override compiling, never called, and
`OD_FRAME_DEFERRED` silently reachable again with nothing failing. Record it as a checked
precondition of this design, not as background.

That is a real RX queue, in RAM the BT stack already charges for (`SL_BT_CONFIG_BUFFER_SIZE`,
3,150 B). **It is NOT drop-free, and an earlier draft said it was.** The buffer is finite, and the
SDK reports discarded buffers, allocation failures and message-creation failures precisely for the
case where the application stops draining events. Holding events is backpressure with a floor, not
storage: § 8's requirement to bound the producer's run is what keeps the hold short enough for the
floor not to be reached, and the hardware rows must include an event flood during CONFIG_READ. A frame is only ever offered to dispatch when dispatch can complete it, so
DEFERRED becomes unreachable in normal operation — and if it is ever returned anyway, that is an
invariant violation the target must **log loudly**, not swallow. Add the log; do not add a retry
buffer.

### 2.3 A bounded 3-slot arena, drained synchronously, that costs less than what it replaces

Shared dispatch cannot link without `od_txq`, and that is structural: reservation is step 5 of six
and precedes the *gate*, because the gate's own `[00][cmd][FE]`/`[00][cmd][FF]` answers need slots
(`shared/core/od_dispatch.h:3-20`); `od_cmd_ctx_t` carries the reservation token
(`shared/core/od_cmd.h:71-74`); every handler spends units through `od_reply`.

**Synchronous drain does not remove the storage, and it is worth being precise about why**, because
"just send it now" is the obvious idea and it fails on two counts:

1. **A handler emits up to two replies inside one dispatch call.** `DIRECT_WRITE_END` sends
   `[00][72]` then `[00][73/74]`, and `od_dispatch_budget()` reserves 2 for it. There is no point
   between them at which the target regains control to drain. Both must exist at once →
   **2 usable slots**, 3 with the ring's empty sentinel.
2. **RETRY requires the bytes be kept.** A queued entry is immutable precisely so a radio RETRY
   re-sends the same bytes and **spends no further nonce** (`shared/core/od_txq.h:48`). Re-sealing
   to retry would burn a counter. Drop the storage and RETRY collapses back into D6.

So: **`OD_TXQ_SLOTS=3`, drained synchronously.** The target calls `od_txq_process()` immediately
after `od_dispatch_frame()` in the same pass.

**"Empty at the end of every pass" is NOT an invariant and must not be written as one.**
`od_txq_process()` stops at the head and keeps the entry on `OD_RADIO_RETRY`, because ordering
forbids skipping past it (`shared/core/od_txq.c:187-189`). A retained entry surviving into the next
pass IS the RETRY design — the same design § 2.3 point 2 relies on so a retry re-sends rather than
re-seals. The property C13 actually guarantees is **bounded**: at most `OD_TXQ_SLOTS - 1` entries,
never growing, with `sl_bt_can_process_event()` refusing new work until they clear.

**Sizing is already settled and C13 does not reopen it.** `PLAN_OD_DISPATCH_2026-08-14.md` §2,
`OD_SESSION_PLAN_2026-08-15.md`, and `docs/ARCHITECTURE.md` distinguish the 256-byte storage width
from the 253-byte GATT value/sealed ceiling. `OD_TX_FRAME_MAX` therefore stays **256** and does not
gain a target override; `OD_SESSION_SEALED_MAX` stays **253**. Collapsing the two numbers would make
storage width look like permission to emit a 256-byte characteristic value.

On the BG22 ABI an entry is origin 4 + tag 4 + len 2 + data 256 plus tail alignment = 268 B.
Confirm the private `s_ring` map symbol is 804 B rather than relying only on field-sum arithmetic.
**3 × 268 = 804 B.** `OD_TXQ_SLOTS` is already `#ifndef`-guarded (`shared/core/od_txq.h:43`), so
this is a `target_compile_definitions` setting and not a shared edit.

**Assert the 3 against the BUDGET, not against a literal.** Nothing in `shared/` floors
`OD_TXQ_SLOTS`, and the other two targets pin it with
`OD_STATIC_ASSERT(OD_TXQ_SLOTS >= PIPE_MAX_W + 2u)` — a rule BG22 cannot state, having no PIPE
window. An earlier draft proposed:

```c
OD_STATIC_ASSERT(OD_TXQ_SLOTS - 1u >= 2u, "a 2-unit reservation must fit the arena");   /* NO */
```

**That is a tautology and it does not do the job.** It compares two literals. A future non-PIPE
opcode given a budget of 3 in `od_dispatch_budget()` (`shared/core/od_dispatch.c:33-45`) still
passes it, then reserves, fails, and returns `OD_FRAME_DEFERRED` on every such frame — the outcome
§ 2.2 has just argued is unreachable, arriving silently.

The assert has to reference the budget table, which means `shared/` must expose its maximum:

```c
/* shared/core/od_dispatch.h -- the largest reservation od_dispatch_budget() can return for a
 * target that compiles out PIPE. Kept beside the table so the two cannot drift. */
#define OD_DISPATCH_BUDGET_MAX_NO_PIPE 2u
```
```c
/* targets/efr32bg22-slc/ -- usable capacity is SLOTS - 1. */
OD_STATIC_ASSERT(OD_TXQ_SLOTS - 1u >= OD_DISPATCH_BUDGET_MAX_NO_PIPE,
                 "the largest reservation must fit the arena");
```

That is a **third** shared change (§ 1), small and mechanical, and it is the difference between a
compile error and a class of frames silently deferring. Whoever edits `od_dispatch_budget()` must
be forced past this constant; if a cheaper construction guarantees that, take it.

**The GATT characteristic caps values at 244 today, and MTU work alone does not lift it.** The
dynamic characteristic is declared with maximum value length `OD_PIPE_MAX_PAYLOAD` = 244
(`opendisplay_ble.c:1777`), and the long-write staging buffer and admission check match it
(`opendisplay_pipe.c:35`, `:1235`). A 253-byte sealed notification is therefore **not deliverable**
regardless of ATT MTU — and note this binds D8: even the corrected 218-byte NFC cap produces a
253-byte sealed frame, so D8's fix does not fit the current declaration either. C13.0 must raise the
characteristic maximum to 253 and decide whether long writes follow, which adds up to 9 static bytes
that § 4.6 must carry. Without it, § 7's "253-byte sealed notification observed intact" row cannot
pass.

**The Silabs stack must also realize ATT MTU 256 explicitly.** `SL_BGAPI_MAX_PAYLOAD_SIZE` bounds the
configured MTU to `payload_size - 7`, so set it to at least **263**. During the system-boot event,
before advertising, call `sl_bt_gatt_server_set_max_mtu(256, &selected)` and require
`selected == 256`. The peer may negotiate lower on an individual connection, so the radio HAL must
still respect the negotiated `ATT_MTU - 3`; the fleet maximum remains MTU 256 / value 253. A vendor
default is not a new frame-size decision.

**And it is a net reduction.** It replaces `s_crypto_payload_buf[513]` (the RFC 3610 staging, which
PSA AEAD makes unnecessary) and the `uint8_t enc[544]` stack local in `pipe_send`
(`opendisplay_pipe.c:516`). Measured against the alternative of *not* adopting dispatch — where the
target still needs a 253 B sealed-output buffer of its own — the incremental cost of the whole
shared command path is **551 B**, and the target's overall ledger stays negative (§ 4.6).

`od_reply` seals into a 253-byte stack local before committing (`shared/core/od_reply.c:38`). That
is shared code, it exists today, and it is 291 B smaller than the `enc[544]` it displaces.

### 2.4 No target-owned resume — so take the shared producer

`od_config_read.c` is the resumable CONFIG_READ producer and sits in the APP_SESSION tier beside
`od_dispatch.c` (`shared/sources.cmake:131-138`). The tier is atomic and C13 takes it.

This is consistent with the constraint rather than contrary to it: what was ruled out is
*target-owned* reorder/resume machinery — transfer state the target must write and maintain. The
producer is shared, is a handful of fields, and replaces a hand-written loop that is currently
losing data.

The arithmetic is why it matters. `MAX_RESPONSE_DATA_SIZE` is 100
(`shared/protocol/opendisplay_protocol.h:890`) and `od_config_read.c` uses `HDR_COMMON 4u` with two
extra bytes on chunk 0 only, so the payload is **94 bytes in chunk 0 and 96 thereafter**
(`shared/core/od_config_read.c:13,39-44`). A 2,048-byte config is therefore
94 + 21 x 96 = 2,110 >= 2,048: **22 notifications**. Twenty-two frames cannot pass
through a 2-slot arena in one dispatch by any arrangement. BG22 today emits all 22 back-to-back
inside one BGAPI event, with no pacing and no wait on `sl_bt_evt_gatt_server_procedure_completed`
(`opendisplay_pipe.c:758-792`), through a `pipe_send_raw` that drops on failure and returns `void`.
**The current behaviour is the bug** (D6), so "keep it" is not a neutral option.

Under C13 the producer emits one chunk per superloop pass as capacity frees, `sl_bt_can_process_event()`
holds incoming commands while it runs (§ 2.2), and a RETRY re-sends the held bytes rather than
losing them. No reorder and no transfer resume is introduced.

**The producer needs a deadline, and shared code does not give it one.** `od_config_read_pump()`
treats `OD_TXQ_FULL` as "try again next pass" and stays active indefinitely
(`shared/core/od_config_read.c:113,130`). On the other two targets that is harmless. Here it is the
predicate holding every BGAPI event, so an egress that never drains means an application that never
processes an event — against a finite 3,150 B stack buffer with a documented drop path (§ 2.2).
**C13.6 must bound the producer's lifetime in the target** — a deadline stamped at start, checked in
the pump, cancelling the read and logging on expiry. § 8 previously said to bound it "if holding
events stalls the connection"; that is backwards. Bound it from the start and treat any observed
stall as a second defect.

### 2.5 `MAX_CONFIG_SIZE` 2048 on BG22 — accepted, with two hard conditions

This re-opens a closed decision and the record should say so plainly: CLAUDE.md decision 12 and
`DIVERGENCE_MATRIX:210` both make 4096 fleet-wide, chosen 2026-07-25 *specifically* to remove a
divergence a host cannot discover. C13 reverts BG22 to 2048 by owner decision, 2026-08-16.

**The saving is the largest single item here.** At 4096 the target pays +2,048 B of NVM3 record and
+2,048 B of read scratch against a 10,576 B heap — more than the whole ~6 KB negotiating margin
(`docs/MEMORY_CONSTRAINTS.md:72-94`). At 2048 none of that is spent, the two mitigations stop being
load-bearing, and the NVM3 max-object-size question does not arise.

**The condition is that BG22 must refuse loudly, never truncate.** The failure this re-opens is real
and has happened: BG22 stored 2048 while the others stored 4096, so a config that fit an nRF was
silently truncated here (`DIVERGENCE_MATRIX:210`). A host cannot interrogate the limit —
`MAX_CONFIG_SIZE` was dropped from the capability-reporting bytes *because* the value had become
uniform (`docs/ARCHITECTURE.md` § "The gap, and a proposed fix"). Therefore:

- a `CONFIG_WRITE` declaring a total above the cap is **rejected at the start frame**, with a NACK,
  before any byte is stored;
- `OD_CONFIG_ASM_REJECTED` means the caller must not touch storage. No path may commit a partial
  record.

#### `od_config_asm` DOES NOT ENFORCE THE CAP TODAY — fix this before anything else

An earlier draft said "`od_config_asm` already does exactly this ... so adopting it *is* the fix,
provided the cap is right." **That is false, and believing it would ship a remotely triggerable
buffer overflow.**

`od_config_asm_start()` bounds the declared total against the *transferable* ceiling and nothing
else (`shared/core/od_config_asm.c:63`):

```c
#define OD_CONFIG_ASM_MAX_TRANSFERABLE ((uint32_t)MAX_CONFIG_CHUNKS * CONFIG_CHUNK_SIZE)  /* 4000 */
if (total <= CONFIG_CHUNK_SIZE || total > OD_CONFIG_ASM_MAX_TRANSFERABLE) {
    return OD_CONFIG_ASM_REJECTED;
}
```

The buffer is `uint8_t buffer[OD_CONFIG_MAX_SIZE]` (`od_config_asm.h:85`). The two numbers are
never compared. Today the code is safe **by coincidence**: 4,096 > 4,000, so the transferable
ceiling is the tighter bound and the buffer can never be exceeded. There is an unstated invariant
holding the module together — `OD_CONFIG_MAX_SIZE >= OD_CONFIG_ASM_MAX_TRANSFERABLE` — and § 2.5
was about to break it.

At `OD_CONFIG_MAX_SIZE == 2048`, a declared total of **2,049..4,000 is accepted**, and the chunk
copy at `od_config_asm.c:110` writes past the end of a 2,048-byte buffer. The attacker input is a
two-byte little-endian length field in the first CONFIG_WRITE frame, on the target with 32 KB of
RAM and no MPU configured for this.

**Both halves are required in C13.0, and both are shared changes:**

```c
/* od_config_asm.c -- the buffer bound, which the transferable ceiling only implied. */
if (total <= CONFIG_CHUNK_SIZE || total > OD_CONFIG_ASM_MAX_TRANSFERABLE ||
    total > OD_CONFIG_MAX_SIZE) {
    return OD_CONFIG_ASM_REJECTED;
}
```
```c
/* od_config_asm.h -- so a future cap cannot silently re-create the hole. */
OD_STATIC_ASSERT(OD_CONFIG_MAX_SIZE >= CONFIG_CHUNK_SIZE * 2u,
                 "a cap below two chunks makes every chunked write unreachable");
```

The `#ifndef` guard on `OD_CONFIG_MAX_SIZE` is therefore **not** a lone constant change: it is a
constant change that requires a logic change to be safe at any value below 4,000. Sections 1 and
4.1 are corrected to say so.

**This also changes what the cap reversal is worth arguing about.** The RAM saving stands on its
own (below). What does not stand is "adopting the shared assembler is itself the safety fix" — the
safety comes from a bound this plan has to add. If that bound is not landed in the same commit as
the cap, **stop**: 2,048 is strictly more dangerous than 4,096 until it exists.

**Shared changes required: this one and its bound above, plus § 2.3's budget constant — three in
total.** (`OD_TXQ_SLOTS` is NOT among them: it is already `#ifndef`-guarded at
`shared/core/od_txq.h:43`, so § 2.3's depth of 3 is a `target_compile_definitions` setting.)
`OD_CONFIG_MAX_SIZE` is a hard `4096u` at `shared/core/od_config_asm.h:67`, and `struct od_config_asm` embeds
`uint8_t buffer[OD_CONFIG_MAX_SIZE]`. It becomes an `#ifndef`-guarded target macro with a documented
floor — the sizing rule this repo already states for `OPENDISPLAY_ZLIB_WINDOW_BITS` and the queue
depths (`docs/MEMORY_CONSTRAINTS.md:149-156`) — set via `target_compile_definitions` on BG22, with
ESP32 and Nordic asserting 4096.

**Buffer unification is mandatory, not an optimisation.** BG22 already owns a `MAX_CONFIG_SIZE`
scratch (`opendisplay_config_buf()`, the pattern `DIVERGENCE_MATRIX:208` calls correct) plus
`s_cfg_chunk`. Adopting `od_config_asm` naively *adds* its 2,048 B buffer beside them. Both must be
retired in the same commit. If they cannot be, stop — two 2 KB config buffers do not fit, and the
plan is wrong about the storage path.

**And retiring the first one is a STORAGE-FORMAT change, not a deletion.** `opendisplay_config_buf()`
does not own a buffer; it returns `&s_cfg_rec.data`, a field of the static NVM3 record
(`opendisplay_config_storage.h:9-15`):

```c
typedef struct {
  uint32_t magic; uint32_t version; uint32_t crc; uint32_t data_len;
  uint8_t  data[MAX_CONFIG_SIZE];
} opendisplay_config_storage_t;                    /* 2,064 B — exactly the ledger's line */
```

`od_config_asm`'s buffer carries no magic, version, crc or data_len, so C13.2 must decide where the
record header lives.

**An on-flash FORMAT change is probably avoidable, and the naive fix does not work.** `nvm3_writeData()`
takes ONE pointer and length, so "keep a header struct and point NVM3 at the asm buffer" cannot be
implemented — the header and payload must be contiguous at write time. But both structures are
already 16 bytes of metadata followed by the payload, so an overlay with `offsetof` assertions can
preserve the existing bytes exactly. Prefer that; it keeps the § 7 read-back row a regression test
rather than a migration. If the overlay cannot be made to hold, the fallback is a format change with
a **read-back-across-reboot of a config written by the previous layout**, and that obligation must be
written into C13.2 before it starts. Either way the 2,064 B in § 4.6 is only recovered if this lands.

Amend CLAUDE.md decision 12 and `DIVERGENCE_MATRIX` 2.7 in C13.0. A decision reversed in code but
not in the documents that state it is how the next reader gets it wrong.

---

## 3. The defects C13 closes

All eight are recorded or verified in the source; none is speculative.

| # | Defect | Evidence | Closed by |
|---|---|---|---|
| **D1** | **Short plaintext commands bypass CCM mid-session.** Once any client authenticates, a <31-byte plaintext command — REBOOT (2 B), DEEP_SLEEP, DIRECT_WRITE_END — executes unencrypted: the length gate only refuses when there is *no* live session | `DIVERGENCE_MATRIX:171` (1.5a); `opendisplay_pipe.c:1242-1255` | the shared gate — `od_session_security_enabled()`, never frame length. Structural, not a hand fix |
| **D2** | **Session never cleared after a config save.** Change the encryption key over an old session and the old session keeps working | `DIVERGENCE_MATRIX:207` (2.4) — `clear_session` sites `:118,431,621,673,1263`, none post-save | **`od_session_clear()`, in Nordic's order** — seal and queue the ACK under the OLD session, save, reload, then clear. **NOT `od_core_reset()`**: it calls `od_txq_reset()` (`od_core.c:20`), which would drop the ACK it just queued, or invalidate the handler's reservation if called earlier. `od_core_reset()` is disconnect teardown. Template: `targets/nordic-zephyr/src/od_cmd_config.c:218-224` |
| **D3** | **Timeout is idle-based**, so a continuously active session never expires | `DIVERGENCE_MATRIX:295` (6.2); `opendisplay_pipe.c:117` | `od_session` — absolute, ms-domain, wrap-safe |
| **D4** | **Replay hole at `diff == 0`.** An exact replay of the last-seen counter skips the window check entirely | `opendisplay_pipe.c:383` — `nonce_counter <= last_seen && diff != 0` | `od_nonce_window` — 256-bit backward bitmap |
| **D5** | **END ack arrives up to 60 s late** — refresh blocks first, acks follow, so clients that time out on the END ack see false failures | `DIVERGENCE_MATRIX:221` (3.3); `opendisplay_display.cpp:854-894` then `opendisplay_pipe.c:707-722` | the § 3.5 pre-refresh drain barrier, via `od_txq_flush()` |
| **D6** | **Notifications dropped on stack-buffer exhaustion**, invisibly to the host — `pipe_send_raw()` does log the failure locally (`:508`), so "silently" means on the wire. The 22-frame CONFIG_READ burst is the suspected trigger; that it *reliably* exhausts this SDK configuration is **unverified and needs the § 7 row to establish it** | `opendisplay_pipe.c:502-512`, burst at `:758-792` | `od_hal_radio`'s `OD_RADIO_RETRY` arm + `od_config_read`'s one-chunk-per-pass producer |
| **D7** | **`0x45 CONFIG_CLEAR` has no case** — silent drop, though the spec lists Silabs | `DIVERGENCE_MATRIX:186` | `od_cmd_app_config_clear` is a required hook, so the *gap* cannot recur. **Link success is not closure**: a defined hook may legally `return OD_CMD_UNKNOWN` and stay silent, so C13.6 also needs corpus vectors asserting a successful clear and its persistence. Record separately that the canonical header contradicts itself — the target legend at `:238` says Silabs has no 0x45, the opcode block at `:367` lists it |
| **D8** | **An NFC read can build a response that cannot be sealed.** `max_out` is `OD_PIPE_MAX_PAYLOAD - 6` = 238, so a 244-byte plaintext frame seals to 273 B — above the 253-byte BLE frame | `opendisplay_pipe.c:928-941`; Nordic capped its equivalent at 218 for exactly this reason (CLAUDE.md § Status) | cap read data at 218 B, matching Nordic (6 + 218 = 224 = `OD_SESSION_PLAIN_FRAME_MAX`, exactly). **Lowering the cap alone is NOT enough**: several record builders truncate to `out_max` and return success (`opendisplay_ble.c:1252,1293,1335,1352`), so a 219-byte record would silently become a truncated 218-byte one rather than erroring. The builders must fail on overflow, or the § 6.1 "219 errors" test asserts something the code never does. Note also that a 253-byte sealed frame still exceeds the characteristic's 244-byte declaration (§ 2.3) |

D1 and D2 are the reason C13 should not wait indefinitely on board availability. D8 was found while
writing this plan and is not yet in `DIVERGENCE_MATRIX`; add it there in C13.0.

Also retired, though not defects: **byte-inferred sealing** — `force_plain` derived from
`data[0]`/`data[1]` (`opendisplay_pipe.c:530-534`). BG22 is the last target carrying it; `od_reply`
decides at the call site, and `tools/check.sh` already ratchets it by symbol on the other two.

---

## 4. Target architecture after C13

### 4.1 Tier consumption

BG22 takes **PURE + HAL_CRYPTO + HAL_RADIO + APP_SESSION**, and declines **APP_RXQ** (§ 2.2),
**HAL_ADV** and **HAL_WDT**.

**HAL_WDT is declined by choice, not by absence, and the record must say so.** The part HAS a
hardware watchdog: `WDOG_PRESENT` / `WDOG_COUNT 1`, `WDOG0` at `0x4A018000`, `WDOG0_IRQn 43`, 16
`PERSEL` periods over `LFRCO`/`LFXO`/`ULFRCO`/`HCLKDIV1024` — long enough on ULFRCO to span the
60 s refresh. The SDK ships `emlib_wdog.slcc` and `hal_wdog.slcc`, so arming it is an SLC
component, not a driver to write. What is true is narrower: `opendisplay-bg22.slcp` requests no
watchdog component and no target source touches `WDOG`. C13 does not change that, but "BG22 has
no watchdog" must not enter the record as a hardware fact — it is a firmware choice, and § 4.5
depends on it being a conscious one.

That is a new combination: Nordic's, minus WDT and RXQ. Add it to `shared/sources.cmake`'s consumer
note. No new tier is created — declining is what the tiers are for.

**Three shared edits, and one of them is logic.** `OD_CONFIG_MAX_SIZE` gains an `#ifndef` guard,
`od_config_asm_start()` gains the buffer bound that makes any cap below 4,000 safe (§ 2.5 — this is
the memory-safety item, not a sizing nicety), and `od_dispatch.h` gains
`OD_DISPATCH_BUDGET_MAX_NO_PIPE` so the arena assert tracks the budget table (§ 2.3).
`OD_TX_FRAME_MAX` remains the fleet-wide 256-byte storage width, and `OD_TXQ_SLOTS` is already
`#ifndef`-guarded, so neither is an edit. Nothing else in `shared/` changes.

### 4.2 Three seams to implement

| Seam | Over | Notes |
|---|---|---|
| `od_hal_crypto` | PSA | Already present: `psa_crypto_init()` at `opendisplay_pipe.c:87`; CMAC, ECB and RNG already PSA. Only the hand-rolled RFC 3610 CCM (`:236-300`) is replaced. Needs `PSA_WANT_ALG_CCM` in the SLC config and the shortened-tag policy `PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 12)` — plain `PSA_ALG_CCM` pins a 16-byte tag and returns `NOT_PERMITTED` on every operation. Nordic proved the shortened form on silicon (nRF52840, 2026-08-15); copy it. Prepared **key slots**, not a key in the struct: the target clears sessions with `memset`, which would strand a live PSA handle in a finite pool. |
| `od_hal_radio` | `sl_bt_gatt_server_send_notification` | The four-valued result is the point. `SL_STATUS_NO_MORE_RESOURCE` → `RETRY`; a dead connection handle → `GONE`; malformed or permanently refused → `ERROR`. **`!s_notify` maps to `ERROR`, NOT `RETRY`** — an earlier draft had it as RETRY and that deadlocks: `s_notify` changes only when the CCC-status event is delivered (`opendisplay_pipe.c:1288`), and § 2.2's hook holds every event while the queue is non-empty, so a client that writes before subscribing would wait forever for an event its own retained entry is blocking. The SDK agrees on the classification — notifications-disabled is a permanent `SL_STATUS_INVALID_STATE`, not transient backpressure. The general rule for this target: **nothing whose only escape is an application event may map to RETRY.** `od_hal_radio_tag_is_live()` compares an instance identity, not the raw handle — BLE handles are reused. |
| `od_session_app` | target statics | Five functions: the session object, the parsed `SecurityConfig`, `od_now_ms()`, the four device-identity bytes, and the report callback. Nordic's 136-line `od_session_app.c` is the template. |

### 4.3 Execution — the superloop keeps its shape

```
sl_main_process_action()
  └─ sl_bt_step()
       ├─ sl_bt_can_process_event()   ← NEW: false while a producer runs or the arena is non-empty
       └─ sl_bt_on_event()
            └─ opendisplay_pipe_handle_gatt_event()
                 ├─ od_dispatch_frame(&reply, frame)
                 ├─ od_core_frame_done(&reply, outcome)
                 └─ od_txq_process()   ← synchronous drain, same pass
app_process_action()
  └─ opendisplay_ble_process()
       ├─ od_txq_process()             ← retries a held RETRY entry
       ├─ od_config_read_pump()        ← one chunk per pass; cancels at its deadline (§ 2.4)
       └─ display_refresh_if_pending() ← the second half of the END split (§ 4.5)
```

`od_dispatch_frame()` is still called from the BGAPI event handler, because on this target that *is*
consumer context. Nothing moves to a thread; there is none.

**Read the two `od_txq_process()` calls together with the hook above them.** The one inside the
handler is what empties the arena in the common case, so the next event is admitted immediately. The
one in `app_process_action()` is the only thing that can clear a RETRY — and while that entry is
held, the hook admits no events at all. Every RETRY classification therefore has to be escapable
WITHOUT an application event (§ 4.2), and the producer has to give up on its own (§ 2.4). Those two
rules are what keep this diagram from being a deadlock.

### 4.4 What the pipe file becomes

`opendisplay_pipe.c` goes from 1,303 lines to transport and pump only — the shape C11 gave Nordic
(1,194 → 200). Commands move with their state into `od_cmd_{device,config,direct,nfc}.c`, each
exporting the one reset that disconnect cleanup calls. All **21** `od_cmd_app_*` hooks are defined — that is
the count `shared/core/od_cmd_app.h` declares, and what ESP32 and Nordic each define today. A
missing one is a link error, which is what makes the D7 *gap* unrepeatable (though not what proves
CONFIG_CLEAR works — see § 3).

**Four more target symbols are required beyond the hooks**, and C13.5/C13.6 must supply them or the
link fails: `od_txq_app_dropped()` (`od_txq.h:125`), `od_core_frame_done()` (`od_cmd.h:93`),
`od_cmd_allow_unauthenticated()` and `od_cmd_mutates_config()` (`od_dispatch.h:75,81`). The last two
are policy, not plumbing: they decide which opcodes cross the gate unauthenticated and which
conflict with a live CONFIG_READ, so BG22's answers must be written down and corpus-checked, not
copied.

Retired outright: the opcode switch (`:1089-1186`); `struct EncryptionSession s_session` and
everything reached through it, including `od_ccm_encrypt`/`od_ccm_ecb` (`:236-300`) and the replay
ring (`:368-405`); byte-inferred sealing (`:530-534`); the inline CONFIG_READ blast (`:758-792`);
`s_crypto_payload_buf[513]`; and `enc[544]`.

### 4.5 The panel refresh stays blocking — but the ACK stops waiting for it

BG22's `opendisplay_display_direct_write_end()` blocks through a refresh of up to 60 s, and C13 does
not change that. Shared dispatch already declares it legal: `od_dispatch_frame()` "does NOT promise
never to block — an END blocks through a panel refresh of up to 60 s inside a target handler"
(`shared/core/od_dispatch.h:18-20`).

What changes is **ordering** (D5). `od_txq_flush()` is the mechanism and it is already
superloop-shaped: one drain attempt per call, `BUSY` if not yet empty, `TIMEOUT` at the deadline with
entries left queued (`shared/core/od_txq.c:209-229`). The END handler loops on it until `OK` or
`TIMEOUT`, then starts the refresh.

**The barrier is a busy-loop on a target with the watchdog switched off, and that pairing is
deliberate.** `od_txq_flush()` makes one drain attempt per call precisely so the caller keeps
control, and its own comment gives the reason: it "keeps the waiting in the loop that also feeds
the watchdog" (`shared/core/od_txq.c:222`). ESP32 and Nordic have that loop; BG22 declines
HAL_WDT (§ 4.1), so it is the one target using this API with no recovery path.

**AN EARLIER DRAFT'S SAFETY ARGUMENT FOR THIS LOOP WAS WRONG, and the correction changes the
design.** It said `sl_bt_run()` runs before the can-process gate, so the stack keeps draining while
the END handler spins. That is true of the *outer* loop and irrelevant here: the handler is called
synchronously from `sl_bt_process_event()` and must RETURN before `sl_bt_step()` can call
`sl_bt_run()` again (`autogen/sl_bluetooth.c:69,84`). A busy-loop inside the handler therefore
retries the same send against a stack that cannot make progress. If the first attempt returns
`NO_MORE_RESOURCE`, the loop spins to its deadline and the ACK still lands after the refresh — D5
unclosed, with a new stall added.

**So D5 needs the target-local split Nordic already has, not a flush loop.**
`opendisplay_display_direct_write_end()` validates, finalises, refreshes and powers down in one
call (`opendisplay_display.cpp:863-889`), which is also why the handler cannot simply ACK first:
it would be acknowledging an END it has not yet validated. Nordic's `od_cmd_direct.c:85-111` splits
prepare from refresh and queues the ACK between them. C13.7 must do the same:

1. split BG22's END into `prepare` (validate + finalise, no refresh) and `refresh`;
2. on `prepare` success, queue the ACK and **return from the handler** so the superloop drains it;
3. begin the refresh on a subsequent pass.

That is a target-local change to a target-local API — it is not the `od_hal_panel`
`refresh_start`/`refresh_busy` rework, which stays out of scope.

**And "sent" is not "on air".** A successful `sl_bt_gatt_server_send_notification()` means the stack
accepted the buffer. The event that reports transmission is
`sl_bt_evt_gatt_server_notification_tx_completed`, and it is **disabled by default** — the plan
previously named `sl_bt_evt_gatt_server_procedure_completed`, which is not the relevant event. § 7's
"ACK observed on air before refresh begins" row must therefore either enable that event and assert
on it, or be satisfied by a sniffer capture. An enqueue log satisfies nothing.

The § 2.2 hold is still bounded — by § 2.4's producer deadline. If C13.7 wants a belt as well,
arming WDOG0 at a period above the refresh bound is an SLC component away (§ 4.1) — a decision,
not a prerequisite.

Converting refresh to `refresh_start`/`refresh_busy` remains the right eventual design and is
explicitly out of scope — an `od_hal_panel` change affecting three targets.

### 4.6 RAM ledger — C13 gives memory back

Signs are as they affect *static* footprint; every static byte comes out of the 10,576 B heap.

Every line below is a PREDICTION from source reading, not a measurement. Several were missing from
the first draft and were added on 2026-08-17 from the linker map
(`cmake_gcc/build/base/opendisplay-bg22.map`).

| Item | Δ static | Note |
|---|---:|---|
| `s_session` 640 B → `od_session` 112 B | **−528** | measured on both other targets |
| `s_crypto_payload_buf[513]` retired | **−513** | PSA AEAD seals in one shot |
| `od_txq` @ 3 slots × 268 B | **+804** | § 2.3; 256-byte storage width, 253-byte BLE value ceiling |
| `s_od_global_config` 844 B → `od_config` gated | **+1** | **only** with `OD_CONFIG_WITH_{TOUCH,BUZZER,WIFI,DATA_EXTENDED}=0`; ungated is +709 against 484 B of slack |
| `od_config_asm` buffer @ 2048 | **+2,064 / −20** | existing storage + chunk state is 2,064 + 20 B, so retiring both is a small CREDIT, not zero — and only if they are retired in the same commit (§ 2.5) |
| `od_rxq` | **0** | declined; the BGAPI event queue serves instead |
| `MAX_CONFIG_SIZE` staying 2048 | **±0** | vs **+4,096** had 4096 been adopted |
| `s_plain_buf[512]` → dispatch's 223 B `s_plain` | **−289** | added 2026-08-17; was missing |
| `s_cfg_read_buf[100]` → ~32 B of producer state | **−68** | added 2026-08-17; was missing |
| `od_txq` indices and state beyond the ring | **+6** | before alignment; the ring is not the whole module |
| PSA key-slot/readiness state vs `s_crypto_ready` | **+5** | Nordic-shaped adapter |
| session report throttling, if Nordic's is copied | **+8** | BG22 may choose otherwise |
| characteristic/long-write widening 244 → 253 (§ 2.3) | **+9** | forced, not optional |
| `enc[544]` stack local | — | see the stack note below |

**Net: roughly −0.6 KB static, and the earlier "−236 B" was wrong** — it omitted the six lines added
above, four of which are credits. The direction is unchanged and the margin is larger, but **only
the final linker map settles it**; treat every figure here as a prediction to be checked, not a
result.

**THE −544 B STACK CLAIM WAS WRONG and is withdrawn.** Retiring `enc[544]` does not free 544 bytes
of peak stack, because what replaces it is live simultaneously: `od_reply()` holds a 253-byte local
(`shared/core/od_reply.c:36`) while calling `od_session_seal()`, which holds 223 more
(`shared/core/od_session.c:627`). The net is a few dozen bytes at best and could be either sign.
`arm-none-eabi-size` cannot measure peak stack at all. Use `-fstack-usage` output plus a runtime
high-water mark, and treat the 2,752 B reservation as the budget to stay inside.

The 10,576 B heap figure this ledger is stated against comes from `docs/MEMORY_CONSTRAINTS.md`; the
current map reports `heap_size = 0x2958` = **10,584 B**. An 8-byte discrepancy changes no decision,
but the map is the authority — reconcile the doc in C13.8.

Net if unification fails: **+1,828 B**, and two 2 KB config buffers coexist — a design failure
whether or not it links. That is the § 8 stop condition.

**Measure `.bss`/`.text` with `arm-none-eabi-size` after every commit** and record it in the commit
message. It is the MIGRATION.md verification bar for this target and the only early warning here.

---

## 5. Commit sequence

Each commit builds headless, leaves all three targets link-complete, and finishes
`tools/check.sh --targets` with zero failures and zero skips.

| Commit | Content | Required proof |
|---|---|---|
| **C13.0a** | **`od_config_asm_start()` gains the `OD_CONFIG_MAX_SIZE` bound (§ 2.5), before any cap moves.** Shared logic change; no target behaviour change at 4096 | The new test FAILS on the pre-fix tree at a simulated 2048 cap and passes after: a declared total of 2,049 is REJECTED with nothing stored. ESP32/Nordic images **byte-identical** — at 4096 the added bound is unreachable |
| **C13.0b** | `OD_CONFIG_MAX_SIZE` becomes an `#ifndef` target macro with a documented floor; BG22 sets 2048 and ESP32/Nordic assert 4096. Retain `OD_TX_FRAME_MAX=256`. Raise the dynamic characteristic's maximum value length 244 → 253 and decide the long-write staging width (§ 2.3). Configure `SL_BGAPI_MAX_PAYLOAD_SIZE >= 263`; set maximum ATT MTU 256 during system boot before advertising and require the selected maximum to equal 256. Add `OD_DISPATCH_BUDGET_MAX_NO_PIPE` (§ 2.3). Amend CLAUDE.md decision 12 and `DIVERGENCE_MATRIX` 2.7; add D8 to the matrix; record the § 2.1 error-code choice | Host test: oversize declared total REJECTED at the start frame with **nothing stored**, at both caps, and specifically at cap+1; a 253-byte BLE reply is queued and 254 is refused while the TX slot remains 256; ESP32/Nordic builds unchanged; boot log records selected MTU 256; no wire byte moved |
| **C13.1** | Add the Silabs build to `tools/check.sh` as a fourth target gate (`DO_SILABS`, folded into `--targets`), under the same skip-is-not-a-pass rule | `--targets` reports 14/0/0 with the toolchain present, and a **skip** without it |
| **C13.2** | Swap config: `od_config_tlv` + `od_config_asm` + `od_config` replace `opendisplay_config_parser.c` and `s_cfg_chunk`; retire `opendisplay_config_buf()` in the same commit — which is an **NVM3 record layout** change, not a buffer deletion (§ 2.5); feature gates off | Host differential vs the retired parser over the corpus; `.bss` delta within § 4.6; **hardware Gate 2, including read-back of a config written by the PREVIOUS layout if any board carries one** |
| **C13.3** | Implement `od_hal_crypto` over PSA and **repoint the existing `od_ccm_encrypt`/`od_ccm_decrypt` call sites at it** (`opendisplay_pipe.c:442,491`) before deleting the hand-rolled CCM — deleting first leaves those callers unresolved and the commit does not link. Add the Silabs arm to the CCM differential | Differential green against `tests/host/session_ccm_reference.inc`; `.text` delta recorded; the commit links standalone; **hardware: authenticate + one encrypted command** |
| **C13.4** | Implement `od_session_app`; swap `od_session` in; delete `struct EncryptionSession`. Closes **D3, D4** | Host session suite incl. the `diff == 0` case, shown failing first; `.bss` −528 confirmed; **hardware Gate 2 incl. reconnect and key replacement** |
| **C13.5** | Implement `od_hal_radio` **including `od_txq_app_dropped()`**; link `od_txq` at `OD_TXQ_SLOTS=3` via `target_compile_definitions`; add the § 2.3 capacity `OD_STATIC_ASSERT`; synchronous drain in the event handler plus a loop-pass drain. Closes the send half of **D6** | Host TXQ suite; RETRY arm exercised against a fake radio, incl. that a retry re-sends identical bytes and spends no nonce; **`!s_notify` classified ERROR, with the write-before-subscribe sequence shown NOT to deadlock**; `.bss` delta recorded; the assert shown FAILING at `OD_TXQ_SLOTS=2`; negotiated-MTU enforcement tested |
| **C13.6** | Split commands into `od_cmd_{device,config,direct,nfc}.c`; define all 21 hooks **plus `od_core_frame_done()`, `od_cmd_allow_unauthenticated()` and `od_cmd_mutates_config()`** (§ 4.4); adopt `od_dispatch_frame()`; `od_core_reset()` on **disconnect only**, with D2 closed by ordered `od_session_clear()` on the save path (§ 3); add `od_config_read` **with the § 2.4 producer deadline** and the `sl_bt_can_process_event()` override. Delete the opcode switch (`:1089-1195`) and byte-inferred sealing. Closes **D1, D2, D6, D7** and settles 0x0080 | A missing hook is a link error — that is the enforcement; corpus gains `dispatch_corpus_silabs` (§ 6.2), incl. CONFIG_CLEAR success/persistence vectors and BG22's unauthenticated/mutates-config answers; DEFERRED-is-logged assertion; producer deadline expiry tested; **hardware Gate 2 full** |
| **C13.7** | Split BG22's display END into prepare/refresh and queue the ACK between them (**D5**, § 4.5) — **not** a flush busy-loop; the NFC 218-byte cap **plus making the record builders fail rather than truncate** (**D8**, § 3) | **hardware: END ack observed on air before refresh begins**, evidenced by `..._notification_tx_completed` (enabled for the test) or a sniffer capture, never an enqueue log; 218 arrives whole, 219 **errors rather than truncating** |
| **C13.8** | Extend the C11 ownership ratchets to this target; update CLAUDE.md status, `DIVERGENCE_MATRIX` resolutions, `docs/TEST_OWNERSHIP.md` | Clean-tree `--targets` at 14/0/0; every closed matrix row edited to say so |

**C13.0a is a prerequisite for everything and touches no target behaviour** — land it first and
alone. C13.0b-C13.2 are then independent of the rest. C13.5 must precede C13.6, because dispatch
cannot link without the egress it reserves against. C13.7 depends on the display API split, which
has no earlier home.

**These commits are revertable latest-first, not independently.** Once C13.6 lands, reverting C13.5
breaks the link. Say so rather than implying otherwise: the MIGRATION.md property being honoured
here is "one subsystem per swap, each hardware-verified before the next", not arbitrary reordering.

---

## 6. Automated verification

### 6.1 Host coverage

Every defect test must be shown **failing on the pre-fix tree first**. A test that passes before the
fix proves nothing.

- **Config:** differential against the retired `opendisplay_config_parser.c` over the corpus,
  including CRC-16 with the two length bytes fed as zeros — all three donors agree, and Silabs alone
  uses the `OD_CONFIG_CRC_*` named constants, which are worth keeping.
- **Cap:** a declared total above 2048 is REJECTED at the start frame and **nothing is stored**; the
  same vector at 4096 is accepted under the ESP32 cap. One test, two cap values. **Pin cap+1
  specifically** (2,049): it is the value the missing bound accepted, and it must be shown failing
  on the pre-C13.0a tree.
- **BLE sizes:** a 253-byte BLE reply is queued; 254 is `OD_TXQ_TOO_LARGE`; the backing TX slot
  remains 256 bytes. Pins transport admission, generation and storage as three distinct bounds.
- **Crypto:** the Silabs arm of the CCM differential against `session_ccm_reference.inc`.
- **Session:** D4 explicitly — an exact replay of the last-seen counter is refused.
- **Egress:** `NO_MORE_RESOURCE` → RETRY → the same entry re-offered and eventually sent, with
  **byte-identical** bytes and no second nonce spent; a dropped connection → GONE → every entry for
  that tag dropped.
- **Producer:** a 2,048-byte config read yields exactly 22 ordered chunks through a 2-slot arena, and
  `sl_bt_can_process_event()` is false for the duration.
- **DEFERRED:** with `od_config_read_active()` forced true, a conflicting command returns DEFERRED
  and the target's handler **logs** rather than dropping silently.
- **D8:** a 219-byte NFC record errors; 218 seals within 253. The cap is exact rather than
  conservative — `6 + 218 = 224 = OD_SESSION_PLAIN_FRAME_MAX` — so pin 219 as the first failure,
  not merely "some large value".
- **Arena capacity:** `OD_TXQ_SLOTS=2` fails the § 2.3 static assert at compile time, and so does
  raising `OD_DISPATCH_BUDGET_MAX_NO_PIPE` to 3 without raising the depth. A build-time check needs a
  build-time demonstration of BOTH directions.
- **Write-before-subscribe:** a command answered while `s_notify` is false must not leave an entry
  retained. With `!s_notify → ERROR` the queue drains; with the rejected `→ RETRY` mapping the fake
  deadlocks. Assert the deadlock exists under the wrong mapping, or the test proves nothing.
- **Producer deadline:** a `od_config_read` whose egress never drains is cancelled and logged at the
  § 2.4 deadline rather than holding events indefinitely.
- **NFC truncation:** a record builder handed a too-small `out_max` **errors**; it must not return
  success with truncated bytes (§ 3, D8).

### 6.2 The corpus gains a third profile

C12 built `dispatch_corpus_portable` and `dispatch_corpus_nordic` as link-isolated executables
because `od_cmd_app_*` is static composition. C13.6 adds **`dispatch_corpus_silabs`** on the same
pattern: production Silabs command translation units against fake BGAPI, NVM3, display and LED seams.

This is where BG22's deliberate divergences stop being prose and become assertions — 0x0052 NACK
unsupported, 0x0076/0x0077 fail-fast, 0x0080 refused, 0x0081/0x0082 silent, the 2048 cap. Each needs
a corpus capability predicate; `cap_pipe` and `cap_config_4k` are new negative predicates under the
`forbids` mechanism C12.0 adopted. C12's rules carry over unchanged: **a `target-production` vector
excluded by a predicate is a failure, not a skip**, and no fake may see an expected reply.

### 6.3 Capture before C13.6 — the time-sensitive item

C12 § 2.5 records this and it expires at C13.6, the first commit that changes BG22's dispatch
behaviour: **capture the untouched dispatcher on a board first**, with complete provenance (target,
SHA, protocol version, panel, host version, transport, date). After that, a shipped BG22's
pre-migration behaviour is no longer observable from this tree.

With no board, that capture cannot happen. Then **record the deadline as consciously skipped** in
`docs/TEST_OWNERSHIP.md` and proceed. What is forbidden is beginning C13.6 under a claim the gate was
met.

### 6.4 Full gate

`tools/check.sh --targets` after every accepted commit and once from a clean tree at C13.8. Required
summary after C13.1: **14 passed, 0 failed, 0 skipped**.

---

## 7. Hardware gates

BG22 has never been flashed from this repo, so the first flash is itself a result. Gate 2 per
`docs/MIGRATION.md` § "Verification bar per subsystem" applies to C13.2, C13.4 and C13.6
independently.

| Row | Required observation | It distinguishes / it cannot |
|---|---|---|
| Boot, advertise, MSD decode | host decodes the MSD with correct battery and temperature | the import itself / nothing about commands |
| Config single write, chunked write, read-back, reboot | ACK precedes reload; bytes survive reboot | C13.2 / not the cap |
| **Oversize config refused** | a declared total of 4096 draws a NACK at the start frame and **stored config is unchanged after** | § 2.5's condition — the row that proves 2048 is safe rather than merely small |
| Authenticate, encrypted command, reconnect, re-authenticate | new session succeeds after reconnect | C13.3 + C13.4 / not the timeout |
| **Session expiry under continuous traffic** | a session driven continuously past the timeout **expires** | D3 — the pre-C13.4 code never expires, so this row fails by construction before the swap |
| **Config save clears the session** | after a config write changing the key, the previous session no longer works | D2 / not key derivation |
| No-session and decrypt failure | `{00,cmd,FE}` and `{00,cmd,FF}` visible in plaintext | the gate and explicit confidentiality |
| **Short plaintext mid-session** | a 2-byte plaintext REBOOT sent while a session is live is **refused** | D1 — the highest-value row here; the old code executes it |
| Unknown opcode; 0x0080; 0x0076 | silence for unknown; `{FF,80,07,00}`; `{FF,76,07,00}` | § 2.1's wire change |
| **CONFIG_READ delivers 22/22 under notification pressure** | every chunk arrives, in order, with no gap, at 2048 bytes stored | D6 — the RETRY arm and the producer together |
| **Direct END ack before refresh** | ACK observed **on air**, timestamped, before the panel begins | D5 — not inferable from an enqueue log; photograph or video the refresh and correlate timestamps |
| **MTU and NFC 218 / 219** | the connection reports negotiated ATT MTU 256; 218 bytes arrive whole in a 253-byte sealed notification; 219 returns an NFC error without truncation | the settled 256/253 link contract and D8. Requires the § 2.3 characteristic widening — at 244 this row cannot pass |
| **Write before subscribing** | a command written before CCC enable is answered once notifications are enabled, and the link does not wedge | the § 2.2 hold against the § 4.2 `!s_notify` classification — the deadlock an earlier draft would have shipped |
| **Event flood during CONFIG_READ** | a 22-chunk read under concurrent writes and a disconnect completes or fails cleanly; no BGAPI buffer exhaustion | § 2.2's hold has a floor, not infinite storage |
| **Oversize config at cap+1** | a declared total of 2,049 draws a NACK and stored config is unchanged | the § 2.5 bound specifically, not merely "some large value" |

**OD-S1 replay injection does not apply** — it is a PIPE test and BG22 has no PIPE. Do not substitute
a direct-write replay and call the row satisfied.

Evidence rules are C12 § 7.1's: board id, exact SHA, tool versions, raw transcript, device log,
per-row PASS/FAIL. A build or a host suite is not on-air evidence.

---

## 8. Risks and stop conditions

- **Two config buffers must never coexist.** If `opendisplay_config_buf()` and `s_cfg_chunk` cannot
  be retired in the commit that introduces `od_config_asm`, stop — the ledger inverts from −236 B to
  +1,828 B and the plan is wrong about the storage path. Note that the first is an NVM3 record field
  rather than a standalone buffer (§ 2.5), so this stop condition is also the plan's largest piece of
  unestimated work.
- **Stop if the § 2.5 bound is not in place before the cap moves.** `od_config_asm` does not enforce
  `OD_CONFIG_MAX_SIZE` today; at 2048 without C13.0a, a declared total of 2,049..4,000 overflows the
  buffer. This is the plan's one memory-safety stop condition and it is not conditional on anything.
- **Stop if 2048 truncates rather than refuses.** That is the exact failure fleet-wide-4096 existed
  to prevent, and it is invisible to the host.
- **Nothing whose only escape is an application event may map to `OD_RADIO_RETRY`.** § 2.2's hook is
  all-or-nothing, so such a mapping deadlocks by construction (§ 4.2).
- **`OD_FRAME_DEFERRED` must never be dropped.** § 2.2's `sl_bt_can_process_event()` override is what
  makes it unreachable; if it is returned anyway, the target logs it as an invariant violation. Do
  not add a retry buffer, and do not swallow it.
- **Do not `#if` inside shared dispatch, and do not write a Silabs-local `od_txq`.** The
  `OD_CONFIG_MAX_SIZE` `#ifndef` sizing guard in C13.0 is the only shared concession this plan makes;
  anything beyond it is a fork wearing a config flag.
- **PSA CCM fails closed if the tag policy is wrong.** Plain `PSA_ALG_CCM` pins 16 bytes and returns
  `NOT_PERMITTED` on every operation. Copy Nordic's proven shortened-tag form.
- **`sl_bt_can_process_event()` is a stack contract, not a free hook.** The producer's run is
  bounded from the start (§ 2.4), not bounded reactively once a stall is observed — the BGAPI buffer
  is finite and documented to discard. If a stall is observed anyway, treat it as a second defect;
  never raise the supervision timeout to hide it.
- **A retry must re-send, never re-seal.** Re-sealing spends a nonce counter for bytes already on the
  wire. The host test in § 6.1 exists for this specifically.
- **The corpus must not become its own oracle.** The Silabs profile obeys C12's rule: fakes get
  semantic knobs, never expected bytes.
- Stop and revise if implementation requires a canonical protocol-header edit, a new opcode or error
  code, or transfer state machines under `shared/`.

---

## 9. What C13 does not do

- no PIPE, reorder queue, or partial-write (0x76) implementation on BG22;
- no RX ring, and no UNBOUNDED TX backlog — the arena holds at most `OD_TXQ_SLOTS - 1` entries and
  cannot grow. It is not empty at the end of every pass: a RETRY retains its entry by design (§ 2.3);
- no `od_hal_adv` or `od_hal_wdt` — those tiers stay declined, the second **by choice** (§ 4.1);
- no `od_hal_panel` interface change. BG22's own display END **is** split into prepare/refresh, which
  is target-local (§ 4.5);
- no transfer, NFC or compression state-machine promotion to `shared/` — that is the unit after C13,
  and it carries the required re-argument of the plain-C decision;
- no change to ESP32 or Nordic BEHAVIOUR. They gain the `OD_CONFIG_MAX_SIZE` assert, and their
  images must come out byte-identical across C13.0a — at a 4,096 cap the new bound in
  `od_config_asm_start()` is unreachable, and that is how the change is shown to be Silabs-only;
- no hardware-verified label derived from build, host, corpus or sanitizer evidence.

---

## 10. Definition of done

- BG22 links PURE + HAL_CRYPTO + HAL_RADIO + APP_SESSION, declining APP_RXQ explicitly in
  `shared/sources.cmake`'s consumer note rather than by omission;
- `od_dispatch.c` owns the opcode map on **all three** targets, so a new opcode is a link error
  everywhere and the fork risk is gone;
- the third config parser, the second hand-rolled CCM and the last byte-inferred sealing are deleted;
- D1-D8 are closed, each with a host test shown failing on the pre-fix tree;
- the 2048 cap refuses oversize configs at the start frame, proven on hardware, and the reversal is
  recorded in CLAUDE.md and `DIVERGENCE_MATRIX`;
- Silabs selects maximum ATT MTU 256 before advertising, a connection negotiates 256 on hardware,
  and a 253-byte sealed notification is observed intact;
- `.bss`/`.text` are recorded per commit and the final figure matches § 4.6's **negative** net;
- `od_config_asm_start()` bounds the declared total against `OD_CONFIG_MAX_SIZE`, and a cap+1 test
  is shown failing on the pre-fix tree — the plan's one memory-safety item;
- the arena depth is a compile-time consequence of `od_dispatch_budget()`'s maximum, not a constant
  derived in prose;
- no `OD_RADIO_RETRY` mapping exists whose escape is an application event;
- the dynamic characteristic admits 253-byte values, and a 253-byte sealed notification is observed;
- D5 is closed by a prepare/refresh split with the ACK queued between, evidenced on air rather than
  from an enqueue log;
- `HAL_WDT` is recorded as declined **by choice**, with the § 4.5 argument for why the flush barrier
  is safe without it stated in the plan rather than assumed;
- `dispatch_corpus_silabs` runs every `target-production` vector this target owns, with no
  predicate-excluded production vector;
- the pre-C13.6 capture was taken, or its skip was recorded as conscious;
- `tools/check.sh --targets` reports 14 passed, 0 failed, 0 skipped from a clean tree;
- every hardware row has a provenance-backed PASS/FAIL, and no row is inferred.

The next planned unit is the transfer-state-machine promotion — direct, partial, PIPE, NFC and
compression — which needs its own design plan and must re-argue the plain-C decision before
`od_xfer_partial.c` or `od_zlib_stream.c` lands.
