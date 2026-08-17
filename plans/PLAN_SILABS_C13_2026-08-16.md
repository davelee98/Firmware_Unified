# Plan: C13 — `efr32bg22-slc` joins the shared command path

**Status:** NOT STARTED. Written 2026-08-16, after C12's software half landed. C12's hardware rows
(H1-H3) remain ACCEPTED-UNRUN and do **not** block this plan.

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
2. **No RX queue, and no TX backlog.** Egress is a 3-slot arena drained in the same superloop
   pass, never carrying frames between passes.
3. **No reorder, no target-owned resume.**
4. **`MAX_CONFIG_SIZE` stays 2048 on BG22**, deliberately re-opening the fleet-wide-4096 decision
   for this target.

One shared constant gains an `#ifndef` guard (§ 2.5). No other shared source changes.

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
compiled-out subsystem." C13 extends it to 0x0080, reusing error code **0x07**; the canonical header
has no "unsupported PIPE" code and is frozen, so record the gap in FOLLOWUPS rather than inventing
one. `0x0081`/`0x0082` return `OD_CMD_UNKNOWN` and stay silent — a NACK to a DATA frame is fatal to
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

**One precondition, and it has no build-time signal.** `sl_bt_can_process_event()` and `sl_bt_step()`
both sit inside `#if !defined(SL_CATALOG_KERNEL_PRESENT)` (`autogen/sl_bluetooth.c`): under an RTOS
"the stack events are processed in a dedicated event processing task, and these functions are not
used at all". BG22 is a superloop with no kernel (CLAUDE.md decision 6) and
`autogen/sl_component_catalog.h` defines no such symbol today — verified — so the override is live.
But adding a kernel component later would leave the override compiling, never called, and
`OD_FRAME_DEFERRED` silently reachable again with nothing failing. Record it as a checked
precondition of this design, not as background.

That is a real RX queue, in RAM the BT stack already charges for (`SL_BT_CONFIG_BUFFER_SIZE`, 3,150 B),
with no drop path. A frame is only ever offered to dispatch when dispatch can complete it, so
DEFERRED becomes unreachable in normal operation — and if it is ever returned anyway, that is an
invariant violation the target must **log loudly**, not swallow. Add the log; do not add a retry
buffer.

### 2.3 No TX backlog — a 3-slot arena, drained synchronously, that costs less than what it replaces

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
after `od_dispatch_frame()` in the same pass, so the arena is empty at the end of every pass and
never carries a backlog — which is the property the constraint was aimed at.

**Sizing is already settled and C13 does not reopen it.** `PLAN_OD_DISPATCH_2026-08-14.md` §2,
`OD_SESSION_PLAN_2026-08-15.md`, and `docs/ARCHITECTURE.md` distinguish the 256-byte storage width
from the 253-byte GATT value/sealed ceiling. `OD_TX_FRAME_MAX` therefore stays **256** and does not
gain a target override; `OD_SESSION_SEALED_MAX` stays **253**. Collapsing the two numbers would make
storage width look like permission to emit a 256-byte characteristic value.

On the BG22 ABI an entry is origin 4 + tag 4 + len 2 + data 256 plus tail alignment = 268 B.
Confirm the private `s_ring` map symbol is 804 B rather than relying only on field-sum arithmetic.
**3 × 268 = 804 B.** `OD_TXQ_SLOTS` is already `#ifndef`-guarded (`shared/core/od_txq.h:43`), so
this is a `target_compile_definitions` setting and not a shared edit.

**Assert the 3, do not just derive it here.** Nothing in `shared/` floors `OD_TXQ_SLOTS`, and the
other two targets pin it with `OD_STATIC_ASSERT(OD_TXQ_SLOTS >= PIPE_MAX_W + 2u)` — a rule BG22
cannot state, having no PIPE window. So BG22 asserts the property that actually matters:

```c
/* Usable capacity is SLOTS - 1, and the largest reservation od_dispatch_budget() makes on a
 * target without PIPE is 2 (CMD_DIRECT_WRITE_{DATA,END}). */
OD_STATIC_ASSERT(OD_TXQ_SLOTS - 1u >= 2u, "a 2-unit reservation must fit the arena");
```

Without it, an opcode that later needs 3 units would not fail to build — it would reserve, fail,
and return `OD_FRAME_DEFERRED` on every such frame, which on this target is the outcome § 2.2 has
just argued is unreachable. A silent invariant violation is exactly what architectural decision 1's
"`_Static_assert` every wire size" exists to prevent.

**The Silabs stack must realize ATT MTU 256 explicitly.** `SL_BGAPI_MAX_PAYLOAD_SIZE` bounds the
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

### 2.5 `MAX_CONFIG_SIZE` 2048 on BG22 — accepted, with one hard condition

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
  before any byte is stored. `od_config_asm` already does exactly this — it is the first item in its
  header's list of what it exists to prevent (`shared/core/od_config_asm.h:12`) — so adopting it *is*
  the fix, provided the cap is right;
- `OD_CONFIG_ASM_REJECTED` means the caller must not touch storage. No path may commit a partial
  record.

**Shared change required, and it is the ONLY one.** (An earlier draft called this "the second";
there is no first. `OD_TXQ_SLOTS` is already `#ifndef`-guarded at `shared/core/od_txq.h:43`, so
§ 2.3's depth of 3 is set with `target_compile_definitions` and touches no shared source.)
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
record header lives and how NVM3 reads and writes a payload that is no longer contiguous with it.
Two shapes work — keep a 16-byte header struct and point NVM3 at the asm buffer, or stage the
header separately around the same payload — and either is a change to the on-flash object layout,
with a **read-back-across-reboot** obligation on top of the § 7 rows. Budget C13.2 accordingly; the
2,064 B in § 4.6 is only recovered if this lands.

Amend CLAUDE.md decision 12 and `DIVERGENCE_MATRIX` 2.7 in C13.0. A decision reversed in code but
not in the documents that state it is how the next reader gets it wrong.

---

## 3. The defects C13 closes

All eight are recorded or verified in the source; none is speculative.

| # | Defect | Evidence | Closed by |
|---|---|---|---|
| **D1** | **Short plaintext commands bypass CCM mid-session.** Once any client authenticates, a <31-byte plaintext command — REBOOT (2 B), DEEP_SLEEP, DIRECT_WRITE_END — executes unencrypted: the length gate only refuses when there is *no* live session | `DIVERGENCE_MATRIX:171` (1.5a); `opendisplay_pipe.c:1242-1255` | the shared gate — `od_session_security_enabled()`, never frame length. Structural, not a hand fix |
| **D2** | **Session never cleared after a config save.** Change the encryption key over an old session and the old session keeps working | `DIVERGENCE_MATRIX:207` (2.4) — `clear_session` sites `:118,431,621,673,1263`, none post-save | `od_core_reset()` on the save path |
| **D3** | **Timeout is idle-based**, so a continuously active session never expires | `DIVERGENCE_MATRIX:295` (6.2); `opendisplay_pipe.c:117` | `od_session` — absolute, ms-domain, wrap-safe |
| **D4** | **Replay hole at `diff == 0`.** An exact replay of the last-seen counter skips the window check entirely | `opendisplay_pipe.c:383` — `nonce_counter <= last_seen && diff != 0` | `od_nonce_window` — 256-bit backward bitmap |
| **D5** | **END ack arrives up to 60 s late** — refresh blocks first, acks follow, so clients that time out on the END ack see false failures | `DIVERGENCE_MATRIX:221` (3.3); `opendisplay_display.cpp:854-894` then `opendisplay_pipe.c:707-722` | the § 3.5 pre-refresh drain barrier, via `od_txq_flush()` |
| **D6** | **Notifications dropped silently** on stack-buffer exhaustion; the 22-frame CONFIG_READ burst is the reliable trigger | `opendisplay_pipe.c:502-512`, burst at `:758-792` | `od_hal_radio`'s `OD_RADIO_RETRY` arm + `od_config_read`'s one-chunk-per-pass producer |
| **D7** | **`0x45 CONFIG_CLEAR` has no case** — silent drop, though the spec lists Silabs | `DIVERGENCE_MATRIX:186` | `od_cmd_app_config_clear` is a required hook — a missing definition is a link error |
| **D8** | **An NFC read can build a response that cannot be sealed.** `max_out` is `OD_PIPE_MAX_PAYLOAD - 6` = 238, so a 244-byte plaintext frame seals to 273 B — above the 253-byte BLE frame | `opendisplay_pipe.c:928-941`; Nordic capped its equivalent at 218 for exactly this reason (CLAUDE.md § Status) | cap read data at 218 B, matching Nordic |

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

One shared edit in total: the `OD_CONFIG_MAX_SIZE` `#ifndef` guard (§ 2.5). `OD_TX_FRAME_MAX` remains
the fleet-wide 256-byte storage width. No shared logic changes.

### 4.2 Three seams to implement

| Seam | Over | Notes |
|---|---|---|
| `od_hal_crypto` | PSA | Already present: `psa_crypto_init()` at `opendisplay_pipe.c:87`; CMAC, ECB and RNG already PSA. Only the hand-rolled RFC 3610 CCM (`:236-300`) is replaced. Needs `PSA_WANT_ALG_CCM` in the SLC config and the shortened-tag policy `PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 12)` — plain `PSA_ALG_CCM` pins a 16-byte tag and returns `NOT_PERMITTED` on every operation. Nordic proved the shortened form on silicon (nRF52840, 2026-08-15); copy it. Prepared **key slots**, not a key in the struct: the target clears sessions with `memset`, which would strand a live PSA handle in a finite pool. |
| `od_hal_radio` | `sl_bt_gatt_server_send_notification` | The four-valued result is the point. `SL_STATUS_NO_MORE_RESOURCE` → `RETRY`; `!s_notify` → `RETRY`, **not** a drop; a dead connection handle → `GONE`; malformed or permanently refused → `ERROR`. `od_hal_radio_tag_is_live()` compares an instance identity, not the raw handle — BLE handles are reused. |
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
       └─ od_config_read_pump()        ← one chunk per pass while active
```

`od_dispatch_frame()` is still called from the BGAPI event handler, because on this target that *is*
consumer context. Nothing moves to a thread; there is none.

### 4.4 What the pipe file becomes

`opendisplay_pipe.c` goes from 1,303 lines to transport and pump only — the shape C11 gave Nordic
(1,194 → 200). Commands move with their state into `od_cmd_{device,config,direct,nfc}.c`, each
exporting the one reset that disconnect cleanup calls. All **21** `od_cmd_app_*` hooks are defined — that is
the count `shared/core/od_cmd_app.h` declares, and what ESP32 and Nordic each define today. A
missing one is a link error, which is what makes D7 unrepeatable.

Retired outright: the opcode switch (`:1089-1186`); `struct EncryptionSession s_session` and
everything reached through it, including `od_ccm_encrypt`/`od_ccm_ecb` (`:236-300`) and the replay
ring (`:368-405`); byte-inferred sealing (`:530-534`); the inline CONFIG_READ blast (`:758-792`);
`s_crypto_payload_buf[513]`; and `enc[544]`.

### 4.5 The panel refresh stays blocking

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

It is safe here for a specific reason, and the reason belongs in the plan rather than in a
reviewer's head: `sl_bt_step()` calls `sl_bt_run()` **before** consulting
`sl_bt_can_process_event()` (`autogen/sl_bluetooth.c:74-80`), so the stack continues to service
the radio and release notification buffers while events are held. A RETRY therefore drains
without needing the application to consume an event first, and neither § 2.2's hold nor this
barrier can starve the other. **If that ordering ever changes, both mechanisms lose their
safety argument at once** — treat it as a checked precondition, not background.

Both loops are still bounded: the flush by `deadline_ms`, the hold by § 8's requirement to bound
the producer's run. If C13.7 wants a belt as well, arming WDOG0 at a period above the refresh
bound is an SLC component away (§ 4.1) — that is a decision, not a prerequisite.

Converting refresh to `refresh_start`/`refresh_busy` remains the right eventual design and is
explicitly out of scope — an `od_hal_panel` change affecting three targets.

### 4.6 RAM ledger — C13 gives memory back

Signs are as they affect *static* footprint; every static byte comes out of the 10,576 B heap.

| Item | Δ static | Note |
|---|---:|---|
| `s_session` 640 B → `od_session` 112 B | **−528** | measured on both other targets |
| `s_crypto_payload_buf[513]` retired | **−513** | PSA AEAD seals in one shot |
| `od_txq` @ 3 slots × 268 B | **+804** | § 2.3; 256-byte storage width, 253-byte BLE value ceiling |
| `s_od_global_config` 844 B → `od_config` gated | **+1** | **only** with `OD_CONFIG_WITH_{TOUCH,BUZZER,WIFI,DATA_EXTENDED}=0`; ungated is +709 against 484 B of slack |
| `od_config_asm` buffer @ 2048 | **+2,064 / 0** | **0 only if `opendisplay_config_buf()` + `s_cfg_chunk` are retired in the same commit** (§ 2.5) |
| `od_rxq` | **0** | declined; the BGAPI event queue serves instead |
| `MAX_CONFIG_SIZE` staying 2048 | **±0** | vs **+4,096** had 4096 been adopted |
| `enc[544]` stack local | — | **−544 peak stack**, against a 2,752 B stack |

**Net if unification succeeds: −236 B static, −544 B stack.** Adopting the whole shared command path
leaves BG22 with *less* static footprint than it has today.

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
| **C13.0** | `OD_CONFIG_MAX_SIZE` becomes an `#ifndef` target macro with a documented floor; BG22 sets 2048 and ESP32/Nordic assert 4096. Retain `OD_TX_FRAME_MAX=256`. Configure Silabs `SL_BGAPI_MAX_PAYLOAD_SIZE >= 263`; set maximum ATT MTU 256 during system boot before advertising and require the selected maximum to equal 256. Amend CLAUDE.md decision 12 and `DIVERGENCE_MATRIX` 2.7; add D8 to the matrix | Host test: oversize declared total REJECTED at the start frame with **nothing stored**, at both caps; a 253-byte BLE reply is queued and 254 is refused by the BLE value ceiling while the TX slot remains 256; ESP32/Nordic builds unchanged; boot log records selected MTU 256; no wire byte moved |
| **C13.1** | Add the Silabs build to `tools/check.sh` as a fourth target gate (`DO_SILABS`, folded into `--targets`), under the same skip-is-not-a-pass rule | `--targets` reports 14/0/0 with the toolchain present, and a **skip** without it |
| **C13.2** | Swap config: `od_config_tlv` + `od_config_asm` + `od_config` replace `opendisplay_config_parser.c` and `s_cfg_chunk`; retire `opendisplay_config_buf()` in the same commit — which is an **NVM3 record layout** change, not a buffer deletion (§ 2.5); feature gates off | Host differential vs the retired parser over the corpus; `.bss` delta within § 4.6; **hardware Gate 2, including read-back of a config written by the PREVIOUS layout if any board carries one** |
| **C13.3** | Implement `od_hal_crypto` over PSA; delete the hand-rolled CCM; add the Silabs arm to the CCM differential | Differential green against `tests/host/session_ccm_reference.inc`; `.text` delta recorded; **hardware: authenticate + one encrypted command** |
| **C13.4** | Implement `od_session_app`; swap `od_session` in; delete `struct EncryptionSession`. Closes **D3, D4** | Host session suite incl. the `diff == 0` case, shown failing first; `.bss` −528 confirmed; **hardware Gate 2 incl. reconnect and key replacement** |
| **C13.5** | Implement `od_hal_radio`; link `od_txq` at `OD_TXQ_SLOTS=3` via `target_compile_definitions`; add the § 2.3 capacity `OD_STATIC_ASSERT`; synchronous drain in the event handler plus a loop-pass drain. Closes the send half of **D6** | Host TXQ suite; RETRY arm exercised against a fake radio, incl. that a retry re-sends identical bytes and spends no nonce; `.bss` +804 confirmed; the assert shown FAILING at `OD_TXQ_SLOTS=2`; negotiated-MTU enforcement tested |
| **C13.6** | Split commands into `od_cmd_{device,config,direct,nfc}.c`; define all 21 hooks; adopt `od_dispatch_frame()` + `od_core_frame_done()` + `od_core_reset()`; add `od_config_read` and the `sl_bt_can_process_event()` override. Delete the opcode switch and byte-inferred sealing. Closes **D1, D2, D6, D7** and the 0x0080 refusal | A missing hook is a link error — that is the enforcement; corpus gains `dispatch_corpus_silabs` (§ 6.2); DEFERRED-is-logged assertion; **hardware Gate 2 full** |
| **C13.7** | The § 3.5 drain barrier on the direct-write END path (**D5**); the NFC 218-byte cap (**D8**) | **hardware: END ack observed on air before refresh begins**; 218 arrives whole, 219 errors |
| **C13.8** | Extend the C11 ownership ratchets to this target; update CLAUDE.md status, `DIVERGENCE_MATRIX` resolutions, `docs/TEST_OWNERSHIP.md` | Clean-tree `--targets` at 14/0/0; every closed matrix row edited to say so |

C13.0-C13.2 are independent of the rest and are worth landing early; C13.5 must precede C13.6,
because dispatch cannot link without the egress it reserves against.

---

## 6. Automated verification

### 6.1 Host coverage

Every defect test must be shown **failing on the pre-fix tree first**. A test that passes before the
fix proves nothing.

- **Config:** differential against the retired `opendisplay_config_parser.c` over the corpus,
  including CRC-16 with the two length bytes fed as zeros — all three donors agree, and Silabs alone
  uses the `OD_CONFIG_CRC_*` named constants, which are worth keeping.
- **Cap:** a declared total above 2048 is REJECTED at the start frame and **nothing is stored**; the
  same vector at 4096 is accepted under the ESP32 cap. One test, two cap values.
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
- **Arena capacity:** `OD_TXQ_SLOTS=2` fails the § 2.3 static assert at compile time. A build-time
  check needs a build-time demonstration; assert it once, in the commit that sets the depth.

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
| **MTU and NFC 218 / 219** | the connection reports negotiated ATT MTU 256; 218 bytes arrive whole in a 253-byte sealed notification; 219 returns an NFC error without truncation | the settled 256/253 link contract and D8 |

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
- **Stop if 2048 truncates rather than refuses.** That is the exact failure fleet-wide-4096 existed
  to prevent, and it is invisible to the host.
- **`OD_FRAME_DEFERRED` must never be dropped.** § 2.2's `sl_bt_can_process_event()` override is what
  makes it unreachable; if it is returned anyway, the target logs it as an invariant violation. Do
  not add a retry buffer, and do not swallow it.
- **Do not `#if` inside shared dispatch, and do not write a Silabs-local `od_txq`.** The
  `OD_CONFIG_MAX_SIZE` `#ifndef` sizing guard in C13.0 is the only shared concession this plan makes;
  anything beyond it is a fork wearing a config flag.
- **PSA CCM fails closed if the tag policy is wrong.** Plain `PSA_ALG_CCM` pins 16 bytes and returns
  `NOT_PERMITTED` on every operation. Copy Nordic's proven shortened-tag form.
- **`sl_bt_can_process_event()` is a stack contract, not a free hook.** If holding events stalls the
  connection or trips a supervision timeout, bound the producer's run instead — do not raise the
  timeout to hide it.
- **A retry must re-send, never re-seal.** Re-sealing spends a nonce counter for bytes already on the
  wire. The host test in § 6.1 exists for this specifically.
- **The corpus must not become its own oracle.** The Silabs profile obeys C12's rule: fakes get
  semantic knobs, never expected bytes.
- Stop and revise if implementation requires a canonical protocol-header edit, a new opcode or error
  code, or transfer state machines under `shared/`.

---

## 9. What C13 does not do

- no PIPE, reorder queue, or partial-write (0x76) implementation on BG22;
- no RX ring, and no TX backlog — the arena is empty at the end of every superloop pass;
- no `od_hal_adv` or `od_hal_wdt` — those tiers stay declined;
- no panel-refresh interface change;
- no transfer, NFC or compression state-machine promotion to `shared/` — that is the unit after C13,
  and it carries the required re-argument of the plain-C decision;
- no change to ESP32 or Nordic behaviour beyond the one `OD_CONFIG_MAX_SIZE` `#ifndef` guard they
  assert against;
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
- the arena depth is a compile-time consequence of `od_dispatch_budget()`'s maximum, not a constant
  derived in prose;
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
