# Plan: `od_dispatch` — one command path for BLE ingress, dispatch and writeback

**Status:** revision 6, 2026-08-14. Not started.
**Depends on:** [OD_SESSION_PLAN_2026-08-15.md](OD_SESSION_PLAN_2026-08-15.md) landing first.
**Prior art:** [DIVERGENCE_MATRIX.md](../docs/DIVERGENCE_MATRIX.md) § 1,
[SHARED_API_DESIGN.md](../docs/SHARED_API_DESIGN.md) § "Layering", `tests/vectors/dispatch.json`.

## Corrections to earlier drafts

**Draft 1 → 2.** Nordic's dispatcher ceiling is 244, not the 509-byte msgq slot, and ESP32 has no
244 check — the divergence was *reversed*. ESP32 already rejects short plaintext mid-session. Nordic
*does* implement PIPE. The replay corpus is one-sided. LAN dispatches synchronously from loop
(`wifi_service.cpp:1624`) and must not share the BLE ring.

**Draft 2 → 3.** Ceilings are origin-specific (LAN DIRECT_WRITE carries 4092 data bytes). Handlers
mutate state before replying, so reservation must precede the handler. `od_hal_radio_tag_is_live()`
restored. LAN must also report frame completion. One blanket `REJECTED` cannot express ESP32's
activity rule. Nordic's END ACK reaches air only via the retry being removed.

**Draft 3 → 4.** Four opcodes emit more than one response. `{OK,NACK,DEFER}` cannot express a
handler-level auth rejection. Decrypt failure does not increment the abuse run. A 244-byte RX slot
makes the promised NACK unreachable. `DEFERRED` is safe only before decrypt. Resumable
`CONFIG_READ` must own the config scratch. D-C needed an envelope-derived bound. The seal-or-plain
predicate diverges on the wire. `od_core_reset()` needed a threading contract.

**Draft 4 → 5** (all verified against source):

| Rev-4 claim | Fact |
|---|---|
| C1: "draining between chunks — the overflow window does not exist" | **Draining frees nothing while `od_hal_radio_send()` returns `RETRY`.** ESP32's `serviceBleTx()` between chunks (`communication.cpp:571`) helps only when the transport is accepting. C1 also *removes Nordic's retry*, which is what currently makes its 44-chunk read work — so C1 as scoped regresses Nordic. |
| § 5: "both the stamp and the abuse counter are gated on `origin == BLE`" | **Only the counter is.** `communication.cpp:998` has no origin check: `!s_frameRejected && commandName(...) && linkIsOwnerWord(...)`. The BLE gate lives inside `noteAuthRejected()` (`:136`). As written, rev 4 would stop refreshing a LAN client's idle clock and disconnect active LAN sessions. |
| D-C bounds the buffers | …but never the **TX entry width** — and the fix is not a wider slot. The definitive `Firmware` implementation shows that sealing adds 29 bytes to its input plain frame (`../Firmware/src/encryption.cpp:810-840`). BLE's whole frame is 256 and its usable value is **253**, so the plain-frame cap is **224**. `Firmware`'s `payload_len > 255` check only prevents the one-byte length from wrapping; no producer can emit that much and it is not a supported frame ceiling. Nordic's NFC read produces a 244-byte plain frame → 273 sealed, which no 256-byte BLE frame can carry; it works only under `CONFIG_BT_L2CAP_TX_MTU=512` (`prj.conf:58`). See § 2. |
| `0x71` worst case is 3 | **2.** `handleDirectWriteData` emits *either* a data ACK *or* calls END, never both (`display_service.cpp:2242-2247`). `0x81` genuinely is 3 — `sendPipeAck()` **and** `directWriteFinishAndRefresh()` both run (`:2793-2797`). |
| Nordic RX admission "509, or narrow to 256" | Undecided is not a specification. Settled in § 2. |
| "gate → reserve → decrypt" | Ambiguous: any gate that can emit `FE`/`FF` needs a slot, so it must run *after* reservation. Ordering restated in § 3.3. |
| Sealed LAN-plain listed as a conforming case | It is a **robustness** case: with security enabled the device serves TLS, not plaintext LAN. Relabelled in § 8. |
| "Reset rejected from a non-consumer context" test | Not testable through a `void` API — the portable core cannot identify the caller's thread. Replaced in § 8. |

**Draft 5 → 6.** Reconciled session and dispatch against the definitive
`../Firmware/src/encryption.cpp:810-840` envelope shape and the actual response producers. Sealing
adds 29 bytes to its input plain frame. The largest supported producer is the capped NFC response:
222 payload bytes, 224 bytes including the response pair, and 253 bytes sealed. The one-byte
length field's theoretical 255 and the old 512/543-byte scratch limits are not frame ceilings.

---

## 1. What exists today

| Concern | `esp32-idf` | `nordic-zephyr` |
|---|---|---|
| BLE ingress | SPSC ring, `PIPE_MAX_W + 2` × 256 B ≈ 8.7 KB, owner `tag` | `K_MSGQ` 40 × 512 B ≈ **20.5 KB**, `gen` |
| LAN ingress | direct from loop, `wifi_service.cpp:1610-1626` | n/a |
| Gate / switch | `communication.cpp:764-857` / `:909-975` | `opendisplay_pipe.c:1387-1440` / `:1220-1385` |
| Egress | 10-slot `{data,len}` ring + `serviceBleTx()` | inline notify, 200 × `k_msleep(1)` |
| Pre-refresh flush | one bounded `serviceBleTx()` pass, **no deadline** (`display_service.cpp:2336`) | implicit, via the retry |
| Seal predicate | on `response[2]` (`communication.cpp:385-397`) | on `data[0]` (`opendisplay_pipe.c:600-607`) |

Matrix § 1.7 records the Nordic queue as 8 × 514 B — pre-import. Correct it.

---

## 2. Ceilings, admission and slot widths *(all settled)*

| Origin | Dispatcher gate | Transport admission | RX slot |
|---|---|---|---|
| BLE, both targets | **244**, `{0xFF,cmd,0xFE}` | **256** | **256** |
| LAN plain / TLS | **4094**, transport-checked | 4094 | n/a — direct dispatch |

**Decision: Nordic narrows BLE admission from 509 to 256**, matching ESP32, and rejects 257+ at the
ATT layer as ESP32 does (`od_ble_nimble.cpp:249` → ATT 0x0D) rather than dropping silently at the
queue (`opendisplay_pipe.c:1447`). Removes a wire divergence a host cannot discover, keeps the
245–256 dispatcher NACK reachable on both, and reclaims ~12 KB of Nordic RX ring.

**RX queue depth:** `PIPE_MAX_W + 2` slots on both, so usable capacity (`SLOTS − 1`) covers a full
window plus END. Assert it, as `command_queue.h:71-73` already does.

**TX entry width: `OD_TX_FRAME_MAX` = `OD_BLE_MAX_FRAME` = 256, and everything must fit inside it.**
256 is the whole BLE frame — opcode(1) + handle(2) + value — so the **usable value is 253**
(`opendisplay_protocol.h:886`). The definitive envelope implementation is
`../Firmware/src/encryption.cpp:810-840`: its input is a complete plain frame
`[status][cmd_echo][payload]`, and its output adds nonce(16) + encrypted length(1) + tag(12), or
29 bytes. The two leading bytes are present in both forms and become AAD; they are not part of
that 29-byte growth.

The resulting session/dispatch contract is:

| Quantity | Contents | Maximum |
|---|---|---:|
| Response/session payload | bytes after the two command/response bytes | **222** |
| Complete plain frame | `[cmd:2][payload]` | **224** |
| CCM plaintext | `[len:1][payload]` | **223** |
| Sealed application value | `[cmd:2][nonce][ciphertext][tag]` | **253** |

The one-byte inner length can represent 255, and definitive `Firmware` defensively rejects values
above it, but no response producer reaches that representational limit. It must not drive shared
buffer sizes or authorize a 286-byte sealed response. The dispatcher rejects a 225-byte plain
frame before calling `od_session_seal()` so no nonce is burned. A wider TX slot cannot raise this
ceiling — it would only move the failure to the radio.

**This exposes a latent Nordic defect, and D-A brings it forward.** The NFC read fills
`max_out = OD_PIPE_MAX_PAYLOAD - 6` = 238 payload bytes after a 6-byte header
(`opendisplay_pipe.c:1076-1078`), i.e. 244 plaintext → **273 sealed**, which exceeds 253. It works
today only because Nordic negotiates `CONFIG_BT_L2CAP_TX_MTU=512` (`prj.conf:58`) — the same
oversized MTU whose RX side D-A narrows to 256. Narrowing admission while leaving TX at 512 would
be incoherent, so this must be settled here:

**Decision: cap the NFC tag data at 218 bytes** (218 tag-data bytes + 4 NFC metadata bytes = the
222-byte session payload; adding `[status][cmd_echo]` makes a 224-byte plain frame and a 253-byte
sealed value) and record it as a Nordic behaviour change. The alternative —
keeping a 512-byte TX MTU for one opcode — reintroduces the wire divergence D-A removes. Larger
tag contents need the chunked NFC path, not an oversized frame. **Check py-opendisplay's NFC read
expectations before landing**, since this shortens a response a deployed host may size against.

PIPE stays forbidden on LAN. **ESP32 behaviour change:** 245–256-byte BLE frames now NACK;
`dispatch.json:123`'s note is corrected in the same commit.

---

## 3. Egress

### 3.1 Entry identity and typed results

```c
typedef enum { OD_RADIO_SENT, OD_RADIO_RETRY, OD_RADIO_GONE, OD_RADIO_ERROR } od_radio_result_t;
od_radio_result_t od_hal_radio_send(od_origin_t o, uint32_t tag, const uint8_t *f, uint16_t len);
bool              od_hal_radio_tag_is_live(od_origin_t o, uint32_t tag);
```

Entries are `{origin, tag, len, data[OD_TX_FRAME_MAX]}`.
**Drain semantics:** `SENT` advance · `RETRY` keep the entry, stop this pass · `GONE` drop every
entry for that tag · **`ERROR` drop this entry only, log, continue** — it is a malformed-call or
stack-refusal condition, never retried (a retry loop on a permanent error is how a drain becomes a
spin) and never grounds for tearing down the tag.

### 3.2 Reservation is a capacity counter *(decided)*

```c
od_txq_status_t od_txq_reserve(uint8_t count);   /* capacity only; no slot address */
void            od_txq_release(uint8_t unused);
od_txq_status_t od_reply(const od_reply_t *rp, const uint8_t *frame, uint16_t len);
od_txq_status_t od_reply_plain(const od_reply_t *rp, const uint8_t *frame, uint16_t len);
```

`available = free_slots − reserved_count`. `od_reply()` seals into the shared encrypt buffer (D-C)
and copies into a slot the counter guarantees is free.

A reserved-but-uncommitted *slot* would be a hole in a FIFO, so the § 3.5 flush would stall on it —
the exact failure the barrier prevents, since `0x72` holds a reservation across a 60 s refresh. A
counter has no holes. Cost is one `memcpy` of ≤`OD_TX_FRAME_MAX` per response. Safe as a plain
counter because every reserve, commit and drain runs on the loop task.

### 3.3 Ordering is normative

> **structural validation + tag liveness → response budget → reserve → auth/decrypt gate → handler**

Reservation precedes the **gate**, not just the handler: the gate itself emits `[0x00][cmd][0xFE]`
and `[0x00][cmd][0xFF]`, so it needs a slot too. It precedes **decrypt** because decrypt advances
the replay window (`communication.cpp:962-966`) and rewrites the ring slot in place
(`command_queue.h:99-101`) — deferring a decrypted frame replays it on re-dispatch, and three
strikes tear down the session (`encryption.cpp:740-746`). **`OD_FRAME_DEFERRED` is returnable only
before decrypt.** The budget is keyed on the outer opcode, which is AAD-authenticated.

| Opcode | Reserve | Basis |
|---|---|---|
| `0x81` PIPE_WRITE_DATA | **3** | worst case: `sendPipeAck()` **and** END ack **and** refresh status (`display_service.cpp:2793-2797`) |
| `0x71` DIRECT_WRITE_DATA | **2** | ack *or* END — never both (`:2242-2247`); END path is ack + refresh status |
| `0x72`, `0x82` END | **2** | END ack, then post-refresh status (`:2326, 2372-2377`) |
| `0x40` CONFIG_READ | see § 3.4 | |
| everything else | 1 | |

The END reservation is **held across the refresh** — free under § 3.2, and why
`DIRECT_WRITE_END` need not become resumable here.

### 3.4 `CONFIG_READ`

**From C3 it is a resumable producer** holding `{od_reply_t copied by value, next_chunk}`, and it
**owns `getConfigScratch()` until it completes**. The current safety argument is pure synchronicity
(`communication.cpp:529-532`); resumption breaks it, because `CONFIG_WRITE` →
`reloadConfigAfterSave()` → `loadGlobalConfig()` overwrites the same buffer (`:634-640`,
`config_parser.cpp:109-113`), splicing two configs into one CRC-valid read-back. **Any
config-mutating command cancels an active read**; also cancel on `od_core_reset()` or tag death; a
second read replaces the first.

**Until C3** (see § 7) it stays synchronous, calls `od_txq_flush()` between chunks — bounded, which
preserves Nordic's current behaviour and gives ESP32 the deadline it lacks — and **checks
`od_reply()`'s status**: on `FULL` after a flush it aborts the read with a NACK rather than
truncating silently. No stage of this plan ever loses config bytes without telling the host.

### 3.5 The pre-refresh drain barrier

```c
od_txq_status_t od_txq_flush(const od_reply_t *rp, uint32_t deadline_ms);
```

Nordic ACKs END **before** the refresh (`opendisplay_pipe.c:838-841`) and only the 200 ms retry
(`:575-588`) gets it on air; ESP32 makes one bounded, deadline-free pass (`display_service.cpp:2336`),
so under backpressure ESP32 delivers late while Nordic drops (`:587`).

**Resolved divergence:** on deadline expiry frames **stay queued** — ESP32's semantics, possibly
late, never dropped.

**Acceptance is conditional, and says so:** the END ACK is required on air before physical refresh
begins *when the transport becomes writable within the deadline*. If it does not, a late ACK is the
specified outcome, not a failure. The test asserts the writable case and records the unwritable one.

### 3.6 The seal-or-plain predicate must be assigned

ESP32 keys on `response[2]` with a carve-out for the 7-byte pipe ACK (`communication.cpp:385-397`);
Nordic keys on `data[0]` (`opendisplay_pipe.c:600-607`). They disagree on the wire: a 2-byte NACK
`{0xFF,0x72}` is **sealed on ESP32** (len < 3 → status 0x00 → encrypts) and **plaintext on Nordic**.

**Decision:** dispatcher owns the predicate at seal time, ESP32's form is authoritative (CLAUDE.md),
Nordic's delta recorded in `DIVERGENCE_MATRIX.md` § 1.5b as part of C1.

---

## 4. Ingress: two doors, one dispatcher

```c
typedef struct { od_origin_t origin; uint32_t tag; } od_reply_t;

od_frame_outcome_t od_dispatch_frame(const od_reply_t *rp, od_span_t frame);
bool od_rxq_push(const uint8_t *frame, uint16_t len, uint32_t tag);   /* BLE producer ctx */
void od_core_process(void);                                           /* loop ctx */

/* CONSUMER CONTEXT ONLY. Never with a peek outstanding (command_queue.h:120-137).
   Nordic's disconnect runs on the BT thread and keeps deferring via s_close_pending
   (opendisplay_pipe.c:1466-1471) rather than calling this directly. */
void od_core_reset(void);
```

**Every caller of `od_dispatch_frame()` must call `od_core_frame_done()`** — BLE drain and LAN entry
alike. In the header, because the LAN path omitting it is a silent policy regression.

`od_core_process()` **does not promise "never blocks"**: END blocks through a refresh of up to 60 s
while transfer handlers are target-local. Bounded by the watchdog, not by this API.

---

## 5. Outcome and handler contract

```c
typedef enum { OD_CMD_OK, OD_CMD_NACK, OD_CMD_AUTH_REJECTED, OD_CMD_DEFER } od_cmd_result_t;

typedef enum {
    OD_FRAME_ACCEPTED, OD_FRAME_HANDLER_NACK,
    OD_FRAME_AUTH_REQUIRED,   /* answered [0x00][cmd][0xFE] — gate OR handler */
    OD_FRAME_CRYPTO_FAILED,   /* decrypt/replay failure, answered [0x00][cmd][0xFF] */
    OD_FRAME_REJECTED_FRAME, OD_FRAME_UNKNOWN_OPCODE, OD_FRAME_STALE_TAG, OD_FRAME_DEFERRED
} od_frame_outcome_t;

void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome);   /* target */
```

`OD_CMD_AUTH_REJECTED` exists because handlers issue auth rejections themselves
(`communication.cpp:614-621`, `:689-697`); without it a TLS client's refused `CONFIG_WRITE` stamps
activity and holds the exclusive slot forever — the bug `:984-988` records as fixed.
`AUTH_REQUIRED` and `CRYPTO_FAILED` are separate because only the first calls `noteAuthRejected()`
(`:806-821` vs `:839-849`); decrypt failure carries the session's own 3-strike rule
(`encryption.cpp:778-782`) and must not also advance the link-level counter.

**Outcome → policy. The activity stamp and the abuse counter have different origin scopes** —
`communication.cpp:998` has no origin test, the BLE gate is inside `noteAuthRejected()` (`:136`,
"counting those would let LAN traffic drop a BLE client"):

| Outcome | Activity stamp (**every origin**, live owner) | Auth-abuse run (**BLE only**, live owner) | Consume RX |
|---|---|---|---|
| `ACCEPTED` | yes | reset | yes |
| `HANDLER_NACK` | **yes** | reset | yes |
| `AUTH_REQUIRED` | no | **increment** | yes |
| `CRYPTO_FAILED` | no | no change | yes |
| `REJECTED_FRAME` / `UNKNOWN_OPCODE` / `STALE_TAG` | no | no change | yes |
| `DEFERRED` | no | no change | **no** |

Both columns require `linkIsOwnerWord(tag)` (`:143`, `:998`). Getting the left column wrong stops
an active LAN client's idle clock and disconnects it mid-session. These gates live target-side in
`od_core_frame_done()`.

Handlers take the reply context explicitly — no current-origin global:

```c
od_cmd_result_t od_cmd_led_activate(const od_reply_t *rp, od_span_t body);
od_cmd_result_t od_cmd_config_read(const od_reply_t *rp);
const struct od_config *od_cmd_config(void);
```

`od_cmd.h` is scaffolding with a shrink schedule in the header. Link-time C functions (decision 1),
no vtable (decision 2).

---

## 6. Divergence decisions

| # | Decision |
|---|---|
| D-A | Origin-specific gates; **Nordic BLE admission narrows to 256** with an ATT-layer reject (§ 2). Correct `dispatch.json:123`. |
| D-B | TX ring with `{origin,tag}`, typed results incl. `ERROR` semantics, counter reservation, § 3.5 barrier. |
| D-C | Shared **decrypt** scratch is sized to `OD_SESSION_PLAIN_MAX` = **223**, the largest supported CCM plaintext `[len:1][payload:222]`. `od_session_open()` accepts at most a 251-byte envelope after the two command bytes and returns at most 222 payload bytes after validating/removing the length prefix. `Firmware`'s 512-byte scratch and 255-byte representational check are defensive implementation details, not producer limits. LAN-plain *unsealed* frames reach 4094 but dispatch in place and never transit this scratch, so it does not need 4 KB — which BG22 could not pay. |
| **D-C2 (new)** | `OD_TX_FRAME_MAX` = `OD_BLE_MAX_FRAME` = **256** (usable value 253). Dispatch and `od_session` accept a complete plain response frame of at most **224**, carrying at most **222 payload bytes**, and seal it into at most **253** bytes (§ 2). The shared encrypt scratch is 253. **Nordic's NFC tag data is capped to 218 bytes** as a consequence. |
| D-D | Typed outcome + the § 5 table, with the two different origin scopes. |
| D-E | **NFC 0x83 is NOT added to ESP32** — header `@targets` reads "NOT Firmware", `communication.cpp:866` records the omission, and `NFC_ERR_*` has no "unsupported" code. **`SHARED_API_DESIGN.md:680` currently says a disabled NFC implementation NACKs — update it.** **Nordic gains 0x52 → `[0xFF][0x52][0x00][0x00]`** (`device_control.cpp:1003`). |
| D-F | RX slot width 256 both targets; depth `PIPE_MAX_W + 2`, asserted. |
| D-G | Dispatcher owns the command banner and quiet-frame predicate. |
| D-H | `sources.cmake` gains a **`HAL_RADIO`** tier; coordinate with the session plan's crypto tier. |
| D-I | Seal-or-plain predicate dispatcher-owned, ESP32 authoritative (§ 3.6). |

---

## 7. Sequence

**C0 — `od_session` lands first.** ESP32 first within each step. Every commit adds its sources to
`shared/sources.cmake`, **and its host tests, in the same commit** — C-side coverage is not deferred
to C5.

| Commit | Content |
|---|---|
| **C1** | `od_hal_radio`, TX ring (`{origin,tag}`, `OD_TX_FRAME_MAX`, typed results, counter reserve/release), § 3.5 barrier, § 3.6 seal predicate. Nordic's blanket inline retry retires — **but `CONFIG_READ` stays synchronous and uses the bounded `od_txq_flush()` between chunks, aborting with a NACK on `FULL`** (§ 3.4). That preserves Nordic's behaviour, gives ESP32 a deadline it never had, and needs no `od_core_process()`. |
| **C2** | Shared BLE RX ring; Nordic admission narrowed to 256. Each target keeps its existing pump and dispatcher, called from the new drain. LAN untouched. |
| **C3** | `od_dispatch.c` — validation, reservation, gate, opcode switch, `od_core_process()` — **together**, one target at a time. Resumable `CONFIG_READ` lands here, where its driver exists. |
| **C4** | Delete `communication.cpp`'s dispatch half and `command_queue.cpp`; shrink `opendisplay_pipe.c`. Update matrix § 1.5b / § 1.7, `SHARED_API_DESIGN.md:680`, `CLAUDE.md`. |
| **C5** | C corpus runner + hardware passes. |

---

## 8. Tests

Landing with the commit that introduces the code, not at C5.

- **Gate matrix** — {security off/on} × {no session, live} × {AUTHENTICATE, FIRMWARE_VERSION, short
  plaintext, sealed, corrupt sealed} × {BLE, LAN plain, LAN TLS} → handler call, reply bytes,
  outcome. Plus BLE 245 → NACK; BLE 257 → ATT reject; LAN DIRECT_WRITE at 4092 data bytes →
  accepted.
- **Frame sizing** — a 224-byte plain response frame (222-byte payload) seals to exactly 253 and
  is accepted; 225 is rejected as `OD_SESSION_SEAL_TOO_LONG` before sealing (so no nonce is
  burned). Nordic: an NFC read returning the maximum 218 tag-data bytes fits; a producer attempting
  219 fails the build's `OD_STATIC_ASSERT`.
- **Robustness (not conforming traffic)** — an inbound sealed LAN-**plain** frame with a 251-byte
  envelope (253 bytes including the command) is accepted; a 252-byte envelope is rejected before
  the cipher is touched. With security on the device serves TLS, so this exercises the shared
  session bound, not a shipping path.
- **Outcome → policy** — one case per § 5 row asserting stamp/abuse/consume, **including an
  accepted LAN command that stamps activity** and a LAN auth rejection that does *not* advance the
  BLE counter.
- **Reservation** — `0x81` auto-complete needs 3 against rings with 3, 2 and 1 free (the last must
  `DEFER` without executing); a reservation held across a simulated 60 s refresh does not stall
  `od_txq_flush`; `DEFERRED` only pre-decrypt with the replay window unadvanced.
- **Egress** — sustained `RETRY` (order preserved, nonce not advanced); `GONE` drops one tag only;
  `ERROR` drops one entry and continues; `TOO_LARGE` vs `FULL`; deadline expiry leaves frames
  queued.
- **`CONFIG_READ`** — C1: aborts with a NACK, never truncates, when the ring stays full through the
  flush deadline. C3: completes across a multi-pass stall; cancelled by an interleaved
  `CONFIG_WRITE`; cancelled on tag death; second read replaces the first.
- **Ring** — wrap, full, empty, stale-tag discard, and the legal reset-racing-a-producer case.
  *Not* "reset from the wrong thread": a `void` API cannot identify its caller. Cover the contract
  instead by asserting Nordic still defers disconnect cleanup through `s_close_pending`.
- **C corpus runner (C5)** — drives `od_dispatch_frame()` against `dispatch.json`, checking the
  `expect.reply` half `replay_vectors.py:18` cannot.

## 9. Hardware acceptance

Per target at C1, C2, C3: encrypted upload; config write → chunk → read-back → reboot → re-parse;
LED/buzzer/`READ_MSD`/`FIRMWARE_VERSION`; auth failure `{0x00,cmd,0xFE}` and ten rejections still
dropping the ESP32 link; unknown opcode silent; 245-byte BLE frame NACKed; **END ACK on air before
physical refresh, given a writable transport within the deadline** (§ 3.5). Nordic also: encrypted
an encrypted NFC read at the new 218-byte cap, delivered whole on a 256-byte frame (D-C2). ESP32 also: PIPE at small `ack_every`, PIPE-over-LAN refused,
TLS-LAN dispatched without CCM, LAN DIRECT_WRITE at 4092-byte chunks, **and an active LAN session
that stays connected across a long idle-plus-traffic cycle** (§ 5).

## 10. Risks

- **§ 3.5 is the regression-prone change** — verify on air, not by reasoning.
- **§ 3.3's ordering is load-bearing** — reserve before gate *and* before decrypt; get it wrong and
  a full TX ring during a PIPE burst tears down the session in three frames.
- **D-A** changes ESP32 behaviour for 245–256-byte BLE frames and narrows Nordic admission.
- **`od_cmd.h` becoming permanent** — mitigated by the shrink schedule and doing `od_xfer_direct` next.

## 11. Out of scope

Transfer state machines, the PIPE window, session crypto, `link_owner` / `session_guard`, the LAN
transport, Silabs, and any new opcode or error code.

**Cross-plan note:** `OD_SESSION_PLAN_2026-08-15.md` uses the same maxima as § 2/D-C: 222-byte
payload, 224-byte plain frame, 223-byte CCM plaintext, 251-byte envelope after the command, and
253-byte sealed value. `Firmware` defines the envelope shape; the actual producers plus transport
define the supported ceiling.
