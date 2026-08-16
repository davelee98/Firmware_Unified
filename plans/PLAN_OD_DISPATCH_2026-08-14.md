# Plan: `od_dispatch` — one command path for BLE ingress, dispatch and writeback

**Status:** revision 9, 2026-08-16. **C8-C11 are landed** — C8-C10 through `a37c04b`, C11 through
the series beginning `40bcc69`, per
[`PLAN_OD_DISPATCH_C11_2026-08-16.md`](PLAN_OD_DISPATCH_C11_2026-08-16.md). Future work is C12.
**Execution gate: ACCEPTED-UNRUN.** No board was available, so the C10 Nordic matrix and the
ESP32-S3 smoke gate were not run, and neither was the C11 exit matrix. Every C9/C10/C11 layer on
both targets is software-verified only, and C12 inherits the full stacked debt. A green
`tools/check.sh --targets` says nothing about radio timing.
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
| Nordic RX value admission "509, or narrow with ATT MTU 256" | Undecided is not a specification. Settled in § 2. |
| "gate → reserve → decrypt" | Ambiguous: any gate that can emit `FE`/`FF` needs a slot, so it must run *after* reservation. Ordering restated in § 3.3. |
| Sealed LAN-plain listed as a conforming case | It is a **robustness** case: with security enabled the device serves TLS, not plaintext LAN. Relabelled in § 8. |
| "Reset rejected from a non-consumer context" test | Not testable through a `void` API — the portable core cannot identify the caller's thread. Replaced in § 8. |

**Draft 5 → 6.** Reconciled session and dispatch against the definitive
`../Firmware/src/encryption.cpp:810-840` envelope shape and the actual response producers. Sealing
adds 29 bytes to its input plain frame. The largest supported producer is the capped NFC response:
222 payload bytes, 224 bytes including the response pair, and 253 bytes sealed. The one-byte
length field's theoretical 255 and the old 512/543-byte scratch limits are not frame ceilings.

**Draft 6 → 7.** Reconciled the plan against landed `od_session`, its post-landing review, and
the fixed target adapters. The target-owned session state now has an explicit app seam; every
`od_session_open`/`seal` result has a wire disposition; PIPE replay/out-of-window remains silent;
authentication and firmware discovery no longer masquerade as ordinary activity; NACK/plaintext
selection is explicit rather than inferred from overloaded bytes; and `CONFIG_READ` becomes
resumable in C8, when backpressure is introduced, rather than two commits later. The Nordic
218-byte NFC read cap is already landed and is now a baseline to preserve, not work for C8.

---

## 1. What exists today

| Concern | `esp32-idf` | `nordic-zephyr` |
|---|---|---|
| BLE ingress | SPSC ring, `PIPE_MAX_W + 2` × 256 B ≈ 8.7 KB, owner `tag` | `K_MSGQ` 40 × 512 B ≈ **20.5 KB**, `gen` |
| LAN ingress | direct from loop, `wifi_service.cpp:1610-1626` | n/a |
| Session owner | `g_session` in `encryption.cpp` | file-static `s_session` in `opendisplay_pipe.c` |
| Gate / switch | `communication.cpp:784-897` / `:934-1023` | `opendisplay_pipe.c:1010-1087` / `:839-1007` |
| Egress | 10-slot `{data,len}` ring + `serviceBleTx()` | inline notify, 200 × `k_msleep(1)` |
| Pre-refresh flush | one bounded `serviceBleTx()` pass, **no deadline** (`display_service.cpp:2336`) | implicit, via the retry |
| Seal predicate | status at `response[2]`, with PIPE-ACK carve-out | landed composite: status at byte 2, hard-NACK byte 0, and PIPE-ACK carve-out |

Matrix § 1.7 records the Nordic queue as 8 × 514 B — pre-import. Correct it.

---

## 2. Ceilings, admission and slot widths *(all settled)*

| Origin | Dispatcher gate | Transport admission | RX slot |
|---|---|---|---|
| BLE, both targets | **244**, `{0xFF,cmd,0xFE}` | **ATT MTU 256 / value 253** | **256** |
| LAN plain / TLS | **4094**, transport-checked | 4094 | n/a — direct dispatch |

**Decision: Nordic narrows BLE value admission from 509 to 253 (ATT MTU 256)**, matching ESP32, and rejects 254+ at the
ATT layer as ESP32 does (`od_ble_nimble.cpp:249` → ATT 0x0D) rather than dropping silently at the
queue (`opendisplay_pipe.c:1447`). Removes a wire divergence a host cannot discover, keeps the
245–253 dispatcher NACK reachable on both, and reclaims ~12 KB of Nordic RX ring. RX storage remains
256 bytes wide; storage width is not value admission.

**RX queue depth:** `PIPE_MAX_W + 2` slots on both, so usable capacity (`SLOTS − 1`) covers a full
window plus END. Assert it at each target integration, as `od_rxq.h` requires.

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

**Landed baseline: Nordic NFC tag data is capped at 218 bytes** (`opendisplay_pipe.c:691-709`):
218 tag-data bytes + 4 NFC metadata bytes = the 222-byte session payload; adding
`[status][cmd_echo]` makes a 224-byte plain frame and a 253-byte sealed value. C9 removes the
oversized Nordic MTU/buffer configuration but does not change this bound. Records above 218 bytes
can still be *written* by the existing chunked write path, but the current protocol has no chunked read; a read of one must
return the existing NFC read error rather than truncate. `py-opendisplay` currently exposes NFC
write but no NFC-read API, so there is no host read-size assumption to preserve. The cap remains a
documented Nordic behaviour change and a hardware acceptance item because it has not run on a
board.

PIPE stays forbidden on LAN. **ESP32 behaviour change:** 245–253-byte BLE values now NACK;
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

### 3.2 Reservation is a capacity counter with an ownership token *(decided)*

```c
typedef struct { uint8_t remaining; } od_tx_reservation_t;

typedef enum {
    OD_TXQ_OK, OD_TXQ_FULL, OD_TXQ_GONE, OD_TXQ_TIMEOUT,
    OD_TXQ_TOO_LARGE,       /* generic hard NACK already queued */
    OD_TXQ_SEAL_FAILED,     /* generic hard NACK already queued */
    OD_TXQ_INVARIANT
} od_txq_status_t;

od_txq_status_t od_txq_reserve(uint8_t count, od_tx_reservation_t *r);
void            od_txq_release(od_tx_reservation_t *r);
od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp,
                         const uint8_t *frame, uint16_t len);
od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len);
```

`available = free_slots − reserved_count`. A successful reserve increments the global counter and
sets `r->remaining`; each reply consumes exactly one unit from both. `od_txq_release()` returns only
that token's unused units. A reply with no remaining unit is an invariant failure, not permission to
borrow another frame's reservation. This matters once a config producer and an incoming command can
coexist across loop passes.

`od_reply()` is the normal protected-response path: it seals when a live CCM session and origin
require it, then copies the final bytes into the slot the token guarantees. `od_reply_plain()` is
the explicit control/error path and never seals. Neither function accepts a BLE value above 253;
`OD_TX_FRAME_MAX == 256` is storage width, not permission to hand a 256-byte value to ATT.
`FULL` is returned by reserve before any handler mutation. `TOO_LARGE` and `SEAL_FAILED` mean the
token was consumed by the generic plaintext hard NACK described in § 3.7; the caller must not emit
a second reply. `TIMEOUT` is a flush result and leaves queued entries intact.

A reserved-but-uncommitted *slot* would be a hole in a FIFO, so the § 3.5 flush would stall on it —
the exact failure the barrier prevents, since `0x72` holds a reservation across a 60 s refresh. A
counter has no holes. Cost is one `memcpy` of ≤`OD_TX_FRAME_MAX` per response. Safe as a plain
counter because every reserve, commit and drain runs on the loop task; the small token supplies the
ownership that a bare global counter did not.

### 3.3 Ordering is normative

> **structural validation + tag liveness + producer conflict check → response budget → reserve
> → auth/decrypt gate → handler**

Reservation precedes the **gate**, not just the handler: the gate itself emits `[0x00][cmd][0xFE]`
and `[0x00][cmd][0xFF]`, so it needs a slot too. It precedes **decrypt** because decrypt advances
the replay window after successful authentication and the dispatch scratch replaces the queued
sealed bytes — deferring a decrypted frame replays it on re-dispatch. **`OD_FRAME_DEFERRED` is
returnable only before decrypt.** The budget is keyed on the outer opcode, which is later verified
as CCM AAD; using an unauthenticated value to reserve at most three slots is bounded and causes no
state mutation. The producer-conflict check is also pre-decrypt: a config-mutating command or
second CONFIG_READ waits behind an active read, so returning `DEFERRED` leaves both the replay
window and the caller's input bytes untouched.

| Opcode | Reserve | Basis |
|---|---|---|
| `0x81` PIPE_WRITE_DATA | **3** | worst case: `sendPipeAck()` **and** END ack **and** refresh status (`display_service.cpp:2793-2797`) |
| `0x71` DIRECT_WRITE_DATA | **2** | ack *or* END — never both (`:2242-2247`); END path is ack + refresh status |
| `0x72`, `0x82` END | **2** | END ack, then post-refresh status (`:2326, 2372-2377`) |
| `0x40` CONFIG_READ | **1 transferred to producer** | first chunk or immediate read error; later chunks reserve one at a time |
| everything else | 1 | |

The END reservation is **held across the refresh** — free under § 3.2, and why
`DIRECT_WRITE_END` need not become resumable here.

### 3.4 `CONFIG_READ`

It becomes a resumable producer in **C8, in the same commit that introduces finite egress
backpressure**. There is no safe synchronous interim: after a flush deadline leaves the queue full,
the NACK promised by revision 6 has nowhere to go.

The producer holds `{od_reply_t copied by value, next_chunk, first reservation}` and owns
`getConfigScratch()` until completion. The dispatcher's initial one-frame reservation is
transferred to it and pays for the first chunk or an immediate read error. After that, each pass
reserves one slot before constructing the next chunk; `FULL` simply leaves the producer pending.
It never truncates, never spins, and never needs a failure frame merely to report backpressure.

Resumption breaks the current scratch-safety argument: `CONFIG_WRITE` →
`reloadConfigAfterSave()` → `loadGlobalConfig()` overwrites the same buffer, which could splice two
configs into one CRC-valid read-back. **While a read is active, any config-mutating command and a
second CONFIG_READ return `DEFERRED` before decrypt/reservation and are retried after the producer
completes.** BLE retains the ring head; the LAN entry retains one input frame and stops reading the
socket until it can retry. This also preserves the first read's declared chunk count—canceling it
after some chunks were already queued would otherwise leave the host waiting forever. Only
`od_core_reset()` or tag death cancels a read; cancellation releases its unused reservation, and
queued entries for the dead tag are discarded by the ordinary `GONE` path.

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

### 3.6 Plaintext is explicit; response bytes are not a type system

The landed Nordic fix proved that inferring disposition from an overloaded byte is unsafe: reading
byte 0 missed `{0x00,cmd,0xFE/0xFF}`, while reading byte 2 mistakes a 7-byte PIPE ACK whose
`highest_seen` happens to be FE/FF for an error. The shared rule is therefore expressed by the call:

- `od_reply_plain()` for AUTHENTICATE, FIRMWARE_VERSION, auth-required, decrypt-failure, every
  hard NACK beginning `0xFF`, and the generic seal-failure NACK;
- `od_reply()` for successful application replies, including all PIPE ACKs;
- TLS-LAN is always emitted plain at the application layer even through `od_reply()`, because TLS
  already protects it.

This adopts `DIVERGENCE_MATRIX.md` § 1.5b's plaintext-NACK resolution and changes ESP32's cases
that its byte-2 heuristic currently seals. No handler may select confidentiality by inspecting its
own payload. Tests pin every polymorphic shape, especially a PIPE ACK with `highest_seen` FE/FF.

### 3.7 `od_session` binding and complete result disposition

The target keeps the singleton and the facts that `od_session` deliberately did not promote. C8
adds a link-time app seam used by shared egress and the ESP32 dispatcher immediately; C10 binds the
same seam to Nordic:

```c
enum od_session_app_op { OD_SESSION_APP_ALIVE, OD_SESSION_APP_AUTH,
                         OD_SESSION_APP_OPEN, OD_SESSION_APP_SEAL };

struct od_session       *od_session_app_state(void);       /* g_session / s_session */
const struct SecurityConfig *od_session_app_security(void);
uint32_t                 od_session_app_now_ms(void);
void                     od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN]);
void                     od_session_app_report(enum od_session_app_op op, int result,
                                               uint16_t cmd,
                                               const struct od_session_report *report);
```

The report callback preserves target logging without importing target log headers into `shared/`.
Nonce failures remain rate-limited per site at five seconds. It is called **before** the dispatcher
takes the PIPE silent-return arm, so loss telemetry cannot disappear again. Existing teardown
continues through each target's session owner; `od_core_reset()` calls `od_session_clear()` through
this seam and never `memset`s the state.

Inbound disposition is exhaustive:

| `od_session_open()` | Wire/action | Outcome |
|---|---|---|
| `OK` | dispatch returned payload | handler-derived |
| `NO_SESSION` | plaintext `{0x00,cmd,0xFE}` | `AUTH_REQUIRED` |
| `REPLAY` with `NONCE_REPLAY`/`NONCE_OUT_OF_WINDOW` and cmd `0x81` | **silence**, consume frame, log first | `CRYPTO_DROPPED` |
| `WRONG_SESSION`, other `REPLAY`, `BAD_TAG`, `BAD_LENGTH`, `SHORT`, `TOO_LONG`, `CRYPTO_ERROR` | plaintext `{0x00,cmd,0xFF}` | `CRYPTO_FAILED` |
| `NO_ROOM` | invariant failure: log, send the same plaintext FF, never read the output scratch | `CRYPTO_FAILED` |

Only `BAD_TAG` spends an integrity strike; dispatch never adds a second strike or increments the
link auth-abuse run for any crypto failure.

Authentication disposition is also exhaustive. `CHALLENGE` returns the core's plaintext 23-byte
reply and `AUTH_CONTROL`; `ESTABLISHED` returns the plaintext 19-byte reply and
`AUTH_ESTABLISHED`; `REJECTED`, `RATE_LIMITED`, `NOT_CONFIGURED`, `MALFORMED`, `EXPIRED`, and
`CRYPTO_ERROR` return the core's plaintext error reply and `AUTH_CONTROL`. The dispatcher always
supplies `OD_SESSION_REPLY_MAX`, a non-null device id and a non-null result length, so `NO_ROOM` and
`BAD_ARGUMENT` are invariants; if reached, log and synthesize plaintext
`{0x00,0x50,AUTH_STATUS_ERROR}` without treating the attempt as established or as link activity.

Outbound disposition is likewise exhaustive. `OK` queues the sealed bytes. `TOO_LONG` and
`NO_ROOM` are producer/invariant failures detected before a nonce and become a plaintext generic
hard NACK `{0xFF,cmd,0x00}` plus a non-OK `od_txq_status_t`. `TOO_SHORT` has no command byte from
which a valid fallback can be built, so it returns `OD_TXQ_INVARIANT`, emits nothing, and leaves the
reservation for the caller's mandatory release. `NO_SESSION` never leaks the original response
plaintext and substitutes the generic hard NACK. `CRYPTO_ERROR` may have spent exactly one counter;
it substitutes the hard NACK and is never retried or resealed. `COUNTER_EXHAUSTED` clears the
session, substitutes the hard NACK, and requires re-authentication on the next command. A queued
sealed entry is immutable: radio `RETRY` spends **no additional nonce**.

---

## 4. Ingress: two doors, one dispatcher

```c
typedef struct { od_origin_t origin; uint32_t tag; } od_reply_t;

od_frame_outcome_t od_dispatch_frame(const od_reply_t *rp, od_span_t frame);
bool od_rxq_push(const uint8_t *frame, uint16_t len, uint32_t tag);   /* BLE producer ctx */
void od_core_process(void);                                           /* loop ctx */

/* CONSUMER CONTEXT ONLY. Never with a peek outstanding (od_rxq.h reset contract).
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
typedef enum { OD_CMD_OK, OD_CMD_NACK, OD_CMD_AUTH_REJECTED } od_cmd_result_t;

typedef enum {
    OD_FRAME_ACCEPTED, OD_FRAME_HANDLER_NACK,
    OD_FRAME_AUTH_CONTROL,     /* challenge or refused handshake; plaintext, not activity */
    OD_FRAME_AUTH_ESTABLISHED, /* plaintext success; resets abuse, still not activity */
    OD_FRAME_DISCOVERY,        /* FIRMWARE_VERSION; plaintext, not activity */
    OD_FRAME_AUTH_REQUIRED,   /* answered [0x00][cmd][0xFE] — gate OR handler */
    OD_FRAME_CRYPTO_FAILED,   /* crypto refusal answered [0x00][cmd][0xFF] */
    OD_FRAME_CRYPTO_DROPPED,  /* PIPE replay/out-of-window: logged, deliberately silent */
    OD_FRAME_REJECTED_FRAME, OD_FRAME_UNKNOWN_OPCODE, OD_FRAME_STALE_TAG, OD_FRAME_DEFERRED
} od_frame_outcome_t;

void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome);   /* target */
```

Handlers cannot defer. The dispatcher resolves capacity and producer conflicts before decrypt, so
once invoked a handler must complete and return one of these three results.

`OD_CMD_AUTH_REJECTED` exists because handlers issue auth rejections themselves
(`communication.cpp:614-621`, `:689-697`); without it a TLS client's refused `CONFIG_WRITE` stamps
activity and holds the exclusive slot forever — the bug `:984-988` records as fixed.
`AUTH_REQUIRED` and `CRYPTO_FAILED` are separate because only the first calls `noteAuthRejected()`
(`:826-839` vs `:862-896`); `od_session` itself counts only `BAD_TAG` toward its three-strike rule,
while nonce loss and engine faults count nothing. Dispatch must not add a second strike or advance
the link-level counter for any of them.

The control outcomes are not cosmetic. ESP32 currently returns from AUTHENTICATE and
FIRMWARE_VERSION before the activity block, so treating either as ordinary `ACCEPTED` would let a
discovery poll hold the exclusive link indefinitely. A successful step-2 authentication does
reset the abuse run, but it still does not stamp activity. Every other authentication result is
`AUTH_CONTROL`; all handshake reply bytes returned by `od_session_authenticate()` use
`od_reply_plain()`.

**Outcome → policy. The activity stamp and the abuse counter have different origin scopes** —
`communication.cpp:998` has no origin test, the BLE gate is inside `noteAuthRejected()` (`:136`,
"counting those would let LAN traffic drop a BLE client"):

| Outcome | Activity stamp (**every origin**, live owner) | Auth-abuse run (**BLE only**, live owner) | Consume RX |
|---|---|---|---|
| `ACCEPTED` | yes | reset | yes |
| `HANDLER_NACK` | **yes** | reset | yes |
| `AUTH_ESTABLISHED` | no | **reset** | yes |
| `AUTH_CONTROL` / `DISCOVERY` | no | no change | yes |
| `AUTH_REQUIRED` | no | **increment** | yes |
| `CRYPTO_FAILED` / `CRYPTO_DROPPED` | no | no change | yes |
| `REJECTED_FRAME` / `UNKNOWN_OPCODE` / `STALE_TAG` | no | no change | yes |
| `DEFERRED` | no | no change | **no** |

Both columns require `linkIsOwnerWord(tag)` (`:143`, `:998`). Getting the left column wrong stops
an active LAN client's idle clock and disconnects it mid-session. These gates live target-side in
`od_core_frame_done()`. An activity stamp performs both pieces of the existing policy:
`od_session_touch(od_session_app_state(), now)` and the target's owner-clock stamp. The first is
diagnostic/idle session state; the second drives the exclusive-link idle timeout. Calling touch
after a successful BLE decrypt is harmlessly idempotent and is required for accepted TLS-LAN
frames, which correctly bypass `od_session_open()`.

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
| D-A | Origin-specific gates; **Nordic BLE value admission narrows to 253 under ATT MTU 256** with an ATT-layer reject (§ 2). Correct `dispatch.json:123`. |
| D-B | TX ring with `{origin,tag}`, typed results incl. `ERROR` semantics, token-owned capacity reservation, § 3.5 barrier. |
| D-C | Shared **decrypt** scratch is sized to `OD_SESSION_PLAIN_MAX` = **223**, the largest supported CCM plaintext `[len:1][payload:222]`. `od_session_open()` accepts at most a 251-byte envelope after the two command bytes and returns at most 222 payload bytes after validating/removing the length prefix. `Firmware`'s 512-byte scratch and 255-byte representational check are defensive implementation details, not producer limits. LAN-plain *unsealed* frames reach 4094 but dispatch in place and never transit this scratch, so it does not need 4 KB — which BG22 could not pay. |
| D-C2 | `OD_TX_FRAME_MAX` = `OD_BLE_MAX_FRAME` = **256** (usable value 253). Dispatch and `od_session` accept a complete plain response frame of at most **224**, carrying at most **222 payload bytes**, and seal it into at most **253** bytes (§ 2). The shared encrypt scratch is 253. Preserve the already-landed Nordic 218-byte NFC read-data cap. |
| D-D | Typed outcome + the § 5 table, including auth/discovery control outcomes and silent PIPE nonce loss. |
| D-E | **NFC 0x83 is NOT added to ESP32** — header `@targets` reads "NOT Firmware", `communication.cpp:866` records the omission, and `NFC_ERR_*` has no "unsupported" code. **`SHARED_API_DESIGN.md:680` currently says a disabled NFC implementation NACKs — update it.** **Nordic gains 0x52 → `[0xFF][0x52][0x00][0x00]`** (`device_control.cpp:1003`). |
| D-F | RX slot width 256 both targets; depth `PIPE_MAX_W + 2`, asserted. |
| D-G | Dispatcher owns the command banner and quiet-frame predicate. |
| D-H | `sources.cmake` gains a **`HAL_RADIO`** tier and the target-owned `od_session_app` binding in § 3.7; the landed crypto HAL tier is unchanged. |
| D-I | Plain-vs-protected is selected explicitly by `od_reply_plain()` / `od_reply()`; all NACKs are plaintext and PIPE ACK contents never affect the choice (§ 3.6). |
| D-J | Every landed `od_session_authenticate`/`open`/`seal` result has the disposition in § 3.7; no default branch may silently alias a new enum member. |

---

## 7. Sequence

The `od_session` prerequisite (C0–C7) is landed. Before C8, flash an ESP32-S3 and close the missing
session hardware gate; otherwise dispatch and egress failures would be stacked on an unverified
mbedTLS integration. This plan continues the sequence as **C8–C12**, with ESP32's vertical adoption
first and Nordic's second. Every commit adds its sources to `shared/sources.cmake` **and its host tests in
the same commit**; run `tools/check.sh --targets` with zero failures and zero skips before each
commit is accepted. C12 adds the cross-corpus runner, not the first C-side coverage.

| Commit | Content |
|---|---|
| **C8** | Shared foundation plus an **ESP32 vertical adoption**: `od_session_app` seam; `od_hal_radio`; TX ring and reservation tokens; explicit plain/protected replies; exhaustive authenticate/open/seal mapping; typed outcomes; § 3.5 barrier; resumable `CONFIG_READ`; and `od_dispatch.c`. The existing ESP RX pump calls shared dispatch and `od_txq_process()`. Nordic remains on its landed dispatcher/inline notify in this commit, so no half-migrated reservation has to hide in a current-origin global. |
| **C9** | Shared BLE RX ring on both targets; delete ESP32 `command_queue.cpp`; narrow Nordic to ATT MTU 256 / value admission 253 and matching ACL buffers. Nordic's existing main-thread pump calls its existing dispatcher from the new ring for this one commit; LAN remains direct. |
| **C10** | **Nordic vertical adoption** of the C8 egress, resumable config producer, session seam and shared dispatcher; compose RX, TX and producer work in `od_core_process()`. Retire Nordic's blanket inline retry only here. Preserve log-before-silence and the control-frame activity policy. |
| **C11** | **LANDED 2026-08-16.** Shared dispatcher owns the opcode map (`od_cmd_app.h`); Nordic command policy split out of `opendisplay_pipe.c`; ESP32 implicit frame-context globals removed; both session objects private behind `od_session_app`; `od_core_reset()` added and adopted; Nordic PIPE returns truthful verdicts; the three crypto/HAL defects below fixed. Two more surfaced and were fixed with it: ESP32 had answered nothing to `CMD_FIRMWARE_VERSION` since C8, and Nordic's device id could be stack residue when `hwinfo` failed. See the [detailed C11 execution plan](PLAN_OD_DISPATCH_C11_2026-08-16.md). |
| **C12** | C corpus runner + hardware passes. |

### 7.1 Landed crypto-HAL defects folded into C11 — **ALL THREE CLOSED, C11.1, 2026-08-16**

Found by the post-merge integration review, deferred here rather than fixed on `main` because C11
rewrites these files anyway. The first is an availability bug, not a hardening item. The
descriptions below are kept as written, because what each was is the reason its test exists; the
resolution follows each.

**Nordic `slot_release()` latches the prepared slot.** `targets/nordic-zephyr/src/od_hal_crypto.c`
returns `OD_HAL_CRYPTO_ERROR` on a `psa_destroy_key` failure **without clearing
`s_slot_ready[slot]`**, and `od_hal_crypto_key_set()` calls it first and bails when it fails. So a
single failed destroy latches the slot: every later handshake returns
`OD_SESSION_AUTH_CRYPTO_ERROR` and the device is **unauthenticatable until reboot**. It also
breaks the contract at `od_hal_crypto.h`, which requires `key_set` to be "idempotent and
self-repairing", and `od_hal_crypto_key_clear()` is `void`, so `od_session_clear()` believes a key
is released that is not.

This is the same latch class removed from the one-shot path in `c0b3206`, sitting in the
prepared-slot path that runs on **every** handshake. Take the same decision: clear the tracking
regardless, accept the leaked slot, report the failure. A reusable slot beats an unreachable one,
and holding the id was already rejected — retrying needs it kept somewhere, and PSA may reissue a
held id to another key.

> **Closed.** That decision, taken exactly: `slot_release()` copies the id to a local, clears the
> tracked id and the ready flag, and only then calls `psa_destroy_key()`. A failure logs "slot
> leaked" and returns ERROR; the next `key_set()` imports cleanly. `tests/host/nordic_crypto_slot_test.c`
> compiles the production HAL against a fake PSA and injects the fault — it is not reproducible on
> a board, which is why hardware cannot own it.

**ESP32's CSPRNG cannot report failure.** `targets/esp32-idf/hal/od_hal_crypto.c` wraps the `void`
`esp_fill_random()` and always returns OK, so `od_session_authenticate`'s "never offer a challenge
the device cannot honour" branch is unreachable on that target while Nordic honours it. Benign
today — a `0x0050` implies a live BLE link, and ESP-IDF's RNG is true while RF is on — but the two
HALs disagree on a contract the header states.

> **Closed.** `psa_generate_random()`, in its own translation unit (`hal/od_hal_crypto_random.c`)
> so a host test can compile the production function against a fake PSA. No fallback to
> `esp_fill_random()` on error, and no sdkconfig change — the symbols were already linked. The
> AEAD stays on classic mbedTLS: routing CCM through `psa_aead_*` is a second backend migration
> with its own hardware gate.

**`od_session_seal()` omits upstream's activity stamp.** `../Firmware/src/encryption.cpp:841`
stamps activity at the end of `encryptResponse()`; the port stamps only in `od_session_open()`, so
an outbound-heavy session's `last_activity_ms` goes stale. No consumer today (the timeout is
absolute and nothing reads the field), but `od_session.h` advertises it as the basis for a target
idle policy, and C11 is where the adapters that would use it are rewritten.

> **Closed.** A successful seal stamps `now_ms` and nothing else does — not a preflight refusal,
> which produced no bytes, and not a cipher error, which may have spent a counter but still put
> nothing on the wire. Every side of that boundary is pinned in `session_test.c`.

### 7.2 Two more defects C11 surfaced

**ESP32 answered nothing to `CMD_FIRMWARE_VERSION`, from C8 until C11.2.** The cutover moved the
pre-gate FIRMWARE_VERSION arm into shared dispatch and left no target case behind it, so the switch
fell through to `OD_CMD_UNKNOWN` and the frame drew no reply. It is the one command a client must be
able to issue before it can authenticate, and a device whose key the host has lost is otherwise
unidentifiable. Found by the link error the per-command seam produces, which is the argument for
that seam: a missing handler is now a build failure rather than a silent capability loss.

**Nordic's device id could be stack residue.** `od_session_app_device_id()` ignores
`hwinfo_get_device_id()`'s status deliberately, but folded a buffer it never initialised, so on that
path the wire-visible identity varied per boot. Zeroed 2026-08-16.

---

## 8. Tests

Landing with the commit that introduces the code, not at C12.

- **Gate matrix** — {security off/on} × {no session, live} × {AUTHENTICATE, FIRMWARE_VERSION, short
  plaintext, sealed, corrupt sealed} × {BLE, LAN plain, LAN TLS} → handler call, reply bytes,
  outcome. Plus BLE value 245 → NACK; BLE value 254 → ATT reject; LAN DIRECT_WRITE at 4092 data bytes →
  accepted.
- **Session-result matrix** — one case for every `od_session_authenticate`, `od_session_open`, and
  `od_session_seal` enum member, checking wire bytes, outcome/status, session clear/retain, counter
  delta, output-scratch access,
  and report callback.
  **PREREQUISITE, and it is not optional: the fake crypto HAL cannot express these cases today.**
  `g_force_status` in `tests/host/session_fake.c` fails *every* subsequent HAL call, so the first
  call swallows the injection and four of the five step-2 crypto-failure paths are unreachable —
  including the all-zero-session-id rejection and both `od_session_clear()` calls that stop a
  half-derived session persisting. A mutation audit confirmed each is currently uncovered. The fake
  needs fail-on-the-Nth-call before this matrix can be written; writing it against today's fake
  produces cases that pass without reaching the code they name. The same applies to the report
  callback: one of nine `struct od_session_report` fields is asserted anywhere today. A compile-time switch ratchet or equivalent fails when a new enum member is
  added without a disposition.
- **PIPE loss** — replay and out-of-window `0x81` consume the RX entry, call the throttled report
  before returning, emit no frame, spend no integrity strike, and leave the transfer alive. The
  same nonce result on `0x71`, and a `BAD_TAG` on either opcode, emits plaintext FF.
- **Frame sizing** — a 224-byte plain response frame (222-byte payload) seals to exactly 253 and
  is accepted; 225 is rejected as `OD_SESSION_SEAL_TOO_LONG` before sealing (so no nonce is
  burned). Nordic: a 218-byte NFC read fits and seals; a stored 219-byte record produces the
  existing runtime NFC read error and never truncates or overruns the response buffer.
- **Robustness (not conforming traffic)** — an inbound sealed LAN-**plain** frame with a 251-byte
  envelope (253 bytes including the command) is accepted; a 252-byte envelope is rejected before
  the cipher is touched. With security on the device serves TLS, so this exercises the shared
  session bound, not a shipping path.
- **Outcome → policy** — one case per § 5 row asserting stamp/abuse/consume, **including an
  accepted LAN command that stamps activity** and a LAN auth rejection that does *not* advance the
  BLE counter. AUTH challenge/refusal and FIRMWARE_VERSION do not stamp; AUTH established resets
  the abuse run without stamping.
- **Reservation** — `0x81` auto-complete needs 3 against rings with 3, 2 and 1 free (the last must
  `DEFER` without executing); a reservation held across a simulated 60 s refresh does not stall
  `od_txq_flush`; one token cannot consume another token's reservation; release returns only unused
  units; `DEFERRED` is possible only pre-decrypt with the replay window unadvanced.
- **Egress** — sustained `RETRY` preserves order and spends exactly one nonce at enqueue, never one
  per retry; `GONE` drops one tag only; `ERROR` drops one entry and continues; usable BLE value 253
  vs slot width 256; `TOO_LARGE` vs `FULL`; deadline expiry leaves frames queued. FE/FF controls,
  every hard NACK, and a PIPE ACK with `highest_seen` FE/FF pin the explicit disposition rule.
- **`CONFIG_READ`** — completes across a multi-pass permanent-then-recovering stall without loss or
  spin; owns its scratch; an interleaved `CONFIG_WRITE` and second read defer pre-decrypt and run
  only after completion; the LAN caller retains its deferred input; tag death/reset cancels and
  releases the token.
- **Session app seam** — target fakes provide state/config/clock/device-id/report independently;
  timeout, auth, open and seal reports reach the callback, and nonce-loss logging precedes silence.
- **Ring** — wrap, full, empty, stale-tag discard, and the legal reset-racing-a-producer case.
  *Not* "reset from the wrong thread": a `void` API cannot identify its caller. Cover the contract
  instead by asserting Nordic still defers disconnect cleanup through `s_close_pending`.
- **C corpus runner (C12)** — drives `od_dispatch_frame()` against `dispatch.json`, checking the
  `expect.reply` half `replay_vectors.py:18` cannot.

## 9. Hardware acceptance

**Before C8:** ESP32-S3 passes the landed `od_session` Gate 2 unchanged: authenticate, encrypted
traffic, timeout/re-authentication, and config persistence. Record the board and commit. This
separates an mbedTLS integration failure from a later dispatch regression.

At C8, run the full matrix below on ESP32. At C9, repeat ingress/admission, encrypted upload and
disconnect/reset coverage on both targets. At C10, run the full matrix on Nordic and repeat the
ESP32 smoke path. The full matrix is: encrypted upload; config write → chunk → read-back → reboot
→ re-parse; a config read surviving induced TX backpressure; LED/buzzer/`READ_MSD`/
`FIRMWARE_VERSION`; no-session `{0x00,cmd,0xFE}` and decrypt-failure `{0x00,cmd,0xFF}` visibly
plaintext; ten BLE auth rejections still dropping the ESP32 link; a successful authentication
clearing a nine-rejection run without stamping the owner activity clock; unknown opcode silent;
245-byte BLE frame NACKed; and **END ACK on air before physical refresh, given a writable transport
within the deadline** (§ 3.5).

At C10/C12 deliberately induce encrypted PIPE loss or reordering on each available board: a replay
or out-of-window `0x81` produces no response, appears in throttled telemetry, and the upload
recovers. This is mandatory because the landed OD-S1 fix has never run on hardware and a normal
encrypted upload does not exercise it.

Nordic also: encrypted NFC read with exactly 218 tag-data bytes delivered whole in a 253-byte
value; a stored 219-byte record returns the NFC read error without truncation. ESP32 also: PIPE at
small `ack_every`, PIPE-over-LAN refused, TLS-LAN dispatched without CCM, LAN DIRECT_WRITE at
4092-byte chunks, and an active LAN session that stays connected across a long
idle-plus-accepted-traffic cycle (§ 5).

## 10. Risks

- **§ 3.5 is the regression-prone change** — verify on air, not by reasoning.
- **§ 3.3's ordering is load-bearing** — reserve before gate *and* before decrypt; get it wrong and
  a full TX ring can consume a replay-window entry for a command that is then neither executed nor
  safely retryable. PIPE loss may be silent, but other commands would still be lost.
- **§ 3.7 is a security-policy seam** — `od_session_report.nonce_reason`, log-before-silence, and
  the no-plaintext-on-seal-failure rule need mutation-style tests, not only happy-path vectors.
- **D-A** changes ESP32 behaviour for 245–253-byte BLE values and narrows Nordic admission.
- **D-I changes ESP32 NACK confidentiality** to the matrix's plaintext rule. Pin host behavior for
  every NACK shape before swapping the target.
- **`od_cmd.h` becoming permanent** — mitigated by the shrink schedule and doing `od_xfer_direct` next.

## 11. Out of scope

Transfer state machines, the PIPE window algorithm, session cryptographic algorithms,
`link_owner` / `session_guard` policy, the LAN transport, Silabs, and any new opcode or error code.
This plan necessarily binds the landed session and calls the existing owner-policy seams; it does
not redesign either subsystem.

**Landed-session note:** `docs/OD_SESSION.md` and `shared/core/od_session.h` are now the source of
truth for the dependency: 222-byte payload, 224-byte plain frame, 223-byte CCM plaintext,
251-byte envelope after the command, and 253-byte sealed value. `Firmware` defines the envelope
shape; the actual producers plus transport define the supported ceiling.
