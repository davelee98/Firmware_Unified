# Plan: `od_dispatch` — one command path for RX queue, dispatch and writeback

**Status:** proposed, 2026-08-14. Not started.
**Scope:** promote the opcode dispatcher, the response-egress path, and the RX/TX command queues
into `shared/`, on `esp32-idf` and `nordic-zephyr`.
**Prior art this plan builds on, not repeats:** [DIVERGENCE_MATRIX.md](DIVERGENCE_MATRIX.md) § 1
(twelve dispatch divergences, already adjudicated) and [SHARED_API_DESIGN.md](SHARED_API_DESIGN.md)
§ "Layering" / § "Internal structure" (the `od_core_rx` / `od_core_process` shape).

---

## 1. What exists today

| Concern | `esp32-idf` | `nordic-zephyr` |
|---|---|---|
| RX queue | `src/command_queue.{h,cpp}` — SPSC ring, `PIPE_MAX_W + 2` slots × `OD_BLE_MAX_FRAME` (256 B) ≈ **8.7 KB**, per-frame owner `tag` | `K_MSGQ_DEFINE(s_pipe_msgq, sizeof(struct od_pipe_msg), 40, 4)` — 40 × 512 B ≈ **20.5 KB**, per-frame `gen` counter |
| Pump | `serviceBleRx()` in `loop()` | `opendisplay_pipe_process()` on `main()` |
| Frame gate + decrypt | `communication.cpp:764-857` | `opendisplay_pipe.c:1387-1440` (`on_pipe_write`) |
| Opcode switch | `communication.cpp:909-975` (20 opcodes) | `opendisplay_pipe.c:1220-1385` (21 opcodes) |
| Response framing | `sendResponse` / `sendResponseUnencrypted`, `communication.cpp:354-432` | `pipe_send` / `pipe_send_raw`, `opendisplay_pipe.c:562-621` |
| Egress | TX ring (10 slots) + `serviceBleTx()` drain, non-blocking, retry next pass | inline `bt_gatt_notify` with **200 × `k_msleep(1)`** retry |
| Origin | `g_commandOrigin` global (BLE / LAN plain / LAN TLS) | BLE only |

Roughly **1 250 lines** of parallel logic. `DIVERGENCE_MATRIX.md` § 1.7 describes the Nordic queue
as "8 × 514 B ≈ 4.1 KB" — that was `Firmware_NRF54` before the import; correct it to 40 × 512 B
when this lands.

### Three divergences found while writing this plan, not in the matrix

- **D-A — maximum inbound frame size is not the same on the two targets.** ESP32 declares
  `OD_BLE_MAX_FRAME` = 256 and the GATT layer rejects anything larger with ATT 0x0D; Nordic
  accepts up to `OD_PIPE_MSG_DATA_MAX` = 509 (ATT MTU 512 − 3) and only NACKs past
  `OD_PIPE_MAX_PAYLOAD`. A host sending a 400-byte frame is served on Nordic and refused on
  ESP32. This is a **wire divergence a host cannot discover**, and the same class of problem
  decision 12 settled for `MAX_CONFIG_SIZE`. It must be decided before the shared ring exists,
  because the ring's slot width *is* the answer.
- **D-B — egress backpressure.** ESP32 queues and retries on the next loop pass; Nordic spins up
  to 200 ms on the main thread inside `pipe_send_raw`. With the watchdog now armed
  (`OD_WDT_TIMEOUT_S` 300 s) that spin is not dangerous, but it is 200 ms of a thread that also
  owns the feed, and it is the only blocking wait in the Nordic command path.
- **D-C — plaintext scratch is duplicated.** ESP32 carries `static uint8_t plaintext[512]` and
  `static uint8_t decrypted_data[512]` (`communication.cpp:825,851`) — 1 KB — plus
  `encrypted_response[600]`; Nordic carries `s_plain_buf[512]` and `s_pipe_enc_buf[544]`. One
  shared decrypt buffer and one shared encrypt buffer saves ~512 B on ESP32 outright and is a
  hard gate for BG22 later.

---

## 2. Target shape

Three headers, matching what `SHARED_API_DESIGN.md` already specifies.

```c
/* shared/core/od_core.h — the whole command surface */
typedef enum { OD_ORIGIN_BLE = 0, OD_ORIGIN_LAN_PLAIN = 1, OD_ORIGIN_LAN_TLS = 2 } od_origin_t;

void od_core_init(void);

/* Producer context: BT RX thread, NimBLE callback, BGAPI handler, LAN task.
   Copies and returns. NO parse, NO crypto, NO panel I/O. `tag` is the target's
   opaque liveness token -- ESP32's packed owner word, Nordic's connection
   generation -- re-checked at dispatch so a frame from a departed peer never runs. */
bool od_core_rx(od_origin_t origin, const uint8_t *frame, uint16_t len, uint32_t tag);

/* Loop context. Drains RX, dispatches, drains TX. Never blocks. */
void od_core_process(void);

/* Discard both rings. Loop context only, from the target's abort path. */
void od_core_reset(void);
```

```c
/* shared/hal/od_hal_radio.h — the egress seam */
int  od_hal_radio_send(od_origin_t origin, const uint8_t *frame, uint16_t len);
bool od_hal_radio_notify_ready(od_origin_t origin);
bool od_hal_radio_connected(od_origin_t origin);
bool od_hal_radio_tag_is_live(uint32_t tag);
```

```c
/* shared/core/od_cmd.h — the handler seam. SCAFFOLDING WITH A SHRINK SCHEDULE. */
void od_cmd_reboot(void);
void od_cmd_enter_dfu(void);
int  od_cmd_direct_write_start(od_span_t body);
int  od_cmd_led_activate(od_span_t body);
/* ...one extern per opcode group... */
const struct od_config *od_cmd_config(void);
```

**Why `od_cmd.h` is not a fourth architectural layer.** The dispatcher's switch calls ~20 handlers
that still live in `targets/` (display, LED, buzzer, config storage, power, DFU). They cannot move
in this change. Each `od_cmd_*` extern is a link-time C function exactly like `shared/hal` —
decision 1, no vtable, no function pointers (decision 2 keeps `od_panel_ops` the only one). The
seam **shrinks on a schedule**: when `od_xfer_direct.c` lands, `od_cmd_direct_write_*` stops being
a target extern and becomes an internal call; same for LED, buzzer, PIPE. Record the shrink
schedule in the header so it cannot quietly become permanent.

**Capability gating**, per `SHARED_API_DESIGN.md`: an opcode a target does not implement is
`#if`-ed to a NACK arm, never a silent drop. Silabs' fail-fast `{FF,76,07,00}` is the model
(matrix § 1, opcode table).

---

## 3. The prerequisite problem, and why dispatch can still go first

The dispatcher's encryption gate needs session state, and `od_session.c` is not promoted — it is
the *next* item after this one. Blocking on it would invert the order the user asked for and make
one enormous commit out of two.

**Resolution: land the interface now, the implementation later.**

```c
/* shared/core/od_session.h — declared now, implemented by target adapters now,
   by shared/core/od_session.c later. od_dispatch.c does not change at that swap. */
bool od_session_security_enabled(void);
bool od_session_alive(void);
bool od_session_decrypt(uint16_t cmd, od_span_t in, uint8_t *out, uint16_t *out_len);
bool od_session_encrypt(od_span_t in, uint8_t *out, uint16_t *out_len);
bool od_session_authenticate(od_span_t body, uint8_t *rsp, uint16_t *rsp_len);
void od_session_clear(void);
```

Adapters are thin: ESP32 maps to `isEncryptionEnabled` / `isAuthenticated` / `decryptCommand` /
`encryptResponse` / `handleAuthenticate` in `encryption.cpp`; Nordic to `sec_enabled` /
`session_alive` / `decrypt_encrypted_payload` / `encrypt_response_payload` /
`authenticate_handle` in `opendisplay_pipe.c`. Both are already byte-identical in behaviour
(matrix § 1.2, and the audit's "verified byte-for-byte identical" finding), so the adapters are
renames, not ports.

**One divergence must be resolved to declare the interface at all.** Nordic's
`authenticate_handle()` *returns* the response for the caller to send; ESP32's
`handleAuthenticate()` sends it itself. **Nordic's shape wins** — a shared dispatcher owns egress,
and a handler that sends behind the dispatcher's back cannot be routed by origin or counted for
the auth-abuse run. ESP32's adapter absorbs the change.

---

## 4. Divergences and their decisions

Rows carried from `DIVERGENCE_MATRIX.md` § 1 keep their numbers; new ones are lettered.

| # | Decision | Rationale |
|---|---|---|
| D-A | **Settle max frame at one value fleet-wide.** Recommend `OD_BLE_MAX_FRAME` = 256 as the contract and Nordic's 509 as the *transport's* ceiling; the shared ring's slot is 256 and the core NACKs above it. | Matches decision 12's reasoning: a uniform value removes a divergence a host cannot discover. 509-byte slots would also cost Nordic 20.5 KB and BG22 everything. **Verify first** that no shipped host sends >256 B frames — this is the one decision here that can break a working deployment. |
| D-B | **Promote the ESP32 TX ring; retire Nordic's 200 ms spin.** | `Firmware` is the authority (CLAUDE.md). Non-blocking with retry-next-pass is strictly better on a thread that also feeds the watchdog. Nordic's config-read path depends on the retry succeeding — the ring must therefore be drained *between* config chunks, which is exactly what ESP32's `handleReadConfig()` already does. |
| D-C | **One decrypt buffer, one encrypt buffer, in `shared/core`.** Sized `OD_MAX_FRAME` + envelope. | Removes ~512 B of duplicate statics on ESP32 and is a precondition for BG22. |
| 1.5a | **Gate on `od_session_security_enabled()`, never on frame length.** Reject short plaintext non-`FIRMWARE_VERSION` commands mid-session. | Nordic's model. Closes the hole where a 2-byte plaintext `REBOOT` executes mid-session. **This is a behaviour change on ESP32** — see § 8. |
| 1.5 | Keep the pre-gate exemptions: `AUTHENTICATE`, `FIRMWARE_VERSION`, and the `rewrite_allowed` config-write path after secure erase. | Unanimous where implemented; Nordic is the most complete. |
| 1.6 | **`origin` is a parameter, never a global.** `g_commandOrigin` dies with `communication.cpp`. | Matrix § 1.6, design doc § `od_hal_radio`. |
| 1.1/1.2/1.4/1.5b | Adopt the shipped reply shapes verbatim: `{0x00,cmd,0xFE}` auth-required, `{0x00,cmd,0xFF}` decrypt-fail, the 4-byte config/LED/buzzer acks, NACKs always plaintext. | Clients parse these. The spec is what is wrong. |
| 1.9 | Unknown opcode: log, **no reply**. | Unanimous. |
| 9.4 | Keep the PIPE-on-LAN refusal (`communication.cpp:885-907`) in the shared dispatcher. | It is a dispatcher obligation, not a transport one. |
| D-D | **Auth-abuse disconnect, activity stamping and owner tags stay in the target.** Dispatch returns a per-frame outcome (`ACCEPTED` / `REJECTED_AUTH` / `REJECTED_OTHER` / `UNKNOWN_OPCODE`) and the target drives its own policy from it. | These are link-ownership policy (`link_owner.cpp`, `session_guard.cpp`), ESP32-only, and depend on multi-instance state Nordic does not have. Moving them would import a whole subsystem this change is not about. The outcome enum is what replaces `s_frameRejected` — and it is a *return value*, not a file-static, so the "read it after the handler" rule the ESP32 comment fought for is structural. |
| D-E | Opcode coverage is the **union**: 0x52 `POWER_OFF` gains a Nordic NACK arm, 0x83 `NFC` becomes reachable on ESP32 behind `OD_NFC_ENABLE`. | Matrix opcode table. Each behind a compile gate; no target pays for what it lacks. |
| D-F | RX ring depth is a target macro (`OD_RX_QUEUE_DEPTH`), derived from `PIPE_MAX_W + 2` where PIPE exists and 1 on BG22. | Design doc § "the ring is target-sized". |
| D-G | The dispatcher owns the per-command log banner and the quiet-frame predicate (`imageWriteLogQuietCmd`). Handlers log no banner. | ESP32 already enforces this by comment; make it structural. |

---

## 5. Sequence

Six commits. Each builds, each is independently revertable, hardware verification between the
ones that change behaviour — per the migration rules. **Nordic adopts first** at every step: it
has no LAN, no PIPE and no auth-abuse machinery, so it exercises the shared path with the fewest
carve-outs. ESP32 second, where the carve-outs get written down.

### S1 — `od_hal_radio` + the shared TX ring
`shared/core/od_txq.c`, `shared/hal/od_hal_radio.h`, implementations in
`targets/esp32-idf/ble/od_ble_nimble.cpp` and `targets/nordic-zephyr/src/opendisplay_ble.c`.
Move `bleTxQueue*` + `serviceBleTx()` out of `command_queue.cpp` unchanged apart from the
`ble.notify()` call becoming `od_hal_radio_send()`. Nordic gains the ring and loses the spin (D-B).
*Verify:* config read (44 back-to-back notifications) completes on both; PIPE ack cadence unchanged
on ESP32 under a small negotiated `ack_every`.

### S2 — the shared RX ring
`shared/core/od_rxq.c` + `od_core_rx()`. ESP32's SPSC ring moves nearly verbatim (it is already
vendor-free by construction). Nordic's `K_MSGQ` is replaced; its `gen` counter becomes the `tag`,
checked through `od_hal_radio_tag_is_live()`. Settle D-A here — the slot width is the decision.
*Verify:* a full PIPE window plus END survives a 60 s panel stall on ESP32; a Nordic upload
completes; RAM delta measured on both.

### S3 — `od_session.h` + target adapters
No behaviour change; pure interface extraction. Includes the `authenticate` return-the-bytes
change (§ 3).
*Verify:* authenticate + encrypted upload on both. This is the cheapest step to get wrong quietly,
so it gets its own hardware pass.

### S4 — `od_dispatch.c`: frame gate only
Header parse, origin gate, the 1.5a security gate, decrypt, the PIPE-on-LAN refusal, the reply
helpers (`od_reply` / `od_reply_plain`, the 2- and 4-byte ack shapes, the NACK namespaces). The
opcode switch still calls the existing target functions through `od_cmd.h`.
*Verify:* the full gate matrix on hardware — see § 7.

### S5 — the opcode switch
The switch itself moves; `imageDataWritten()` and Nordic's `dispatch()` become empty. Opcode
coverage becomes the union (D-E). `od_core_process()` replaces `serviceBleRx()` and
`opendisplay_pipe_process()`.

### S6 — delete and document
Remove `communication.cpp`'s dispatch half and `command_queue.cpp`; shrink
`opendisplay_pipe.c` to its remaining handlers. Update `DIVERGENCE_MATRIX.md` § 1 (including the
4.1 KB → 20.5 KB correction), `sources.cmake`, `CLAUDE.md`.

---

## 6. Host tests

`tests/host/dispatch_test.c`, with fakes for `od_session`, `od_cmd` and `od_hal_radio` (the last
capturing emitted frames for byte comparison).

- **Gate matrix, table-driven.** The cross product of {security off, security on} ×
  {no session, live session} × {`AUTHENTICATE`, `FIRMWARE_VERSION`, short plaintext, long
  plaintext, well-formed encrypted, corrupt encrypted} × {BLE, LAN plain, LAN TLS} →
  expected handler call and expected reply bytes. This table *is* the specification of § 4's
  security decisions, and it is the artefact that makes 1.5a reviewable.
- **Differential reply shapes.** Assert the exact bytes both shipped implementations emit for
  auth-required, decrypt-fail, the 2- and 4-byte acks, and the PIPE-on-LAN NACK. Same rule as
  `advert_test.c`: these encode what clients parse — **do not "fix" them to match the spec.**
- **Ring tests.** Wrap, full, empty, tag-discard, `od_core_reset()` racing a producer (the
  acquire/release argument in `command_queue.h` is currently untested).
- **Corpus.** Drive `tests/host/replay_vectors.py` through `od_core_rx` + `od_core_process`
  instead of the current entry point — the wire corpus becomes a dispatcher test for free.

---

## 7. Hardware acceptance

Per target, per behaviour-changing step (S1, S2, S4, S5):

1. Encrypted image upload completes (`xiao_nrf52840` and an S3).
2. Config write → chunked write → read-back → reboot → re-parse.
3. LED, buzzer, `READ_MSD`, `FIRMWARE_VERSION` unauthenticated.
4. Auth failure answers `{0x00,cmd,0xFE}`; ten consecutive rejections still drop the ESP32 link.
5. **Short plaintext `REBOOT` mid-session is refused** (the 1.5a fix — the device must stay up).
6. Unknown opcode: no reply, link survives.
7. ESP32 only: PIPE transfer at small `ack_every`; PIPE frame over LAN refused; LAN TLS frame
   dispatches without CCM.

---

## 8. Risks

- **1.5a changes ESP32 behaviour on a hardware-verified target.** Short plaintext commands that
  work today mid-session will start being refused. Correct, and a security fix — but check
  `py-opendisplay` and the HA integration for any path that sends a short plaintext frame after
  authenticating, *before* S4. If one exists, it is a host bug, but it needs a coordinated
  release rather than a surprise.
- **D-A can break a deployed host.** Capping frames at 256 is right, but Nordic has been
  accepting 509 since import. Confirm nothing sends larger. If something does, the cap becomes a
  Nordic-only transport ceiling and the wire divergence stays — recorded, not silently kept.
- **`od_cmd.h` becoming permanent.** Twenty externs is a lot of seam. Mitigation: the shrink
  schedule in the header, and `od_xfer_direct` immediately after this so the first four entries
  leave quickly.
- **The RX ring is the one piece of genuinely concurrent code here.** ESP32's version has a
  carefully argued acquire/release contract and no tests. S2 must land the tests before the swap,
  not after.
- **Scope creep into `od_session`.** S3 extracts an interface and nothing else. The KDF, nonce
  window and CCM envelope stay where they are until their own promotion.

---

## 9. Explicitly out of scope

Transfer state machines (0x70/0x71/0x72, 0x76), the PIPE window and reorder buffer, the session
crypto itself, `link_owner` / `session_guard` multi-instance arbitration, the LAN transport, and
any new opcode or error code — the canonical headers are frozen, and
`SHARED_API_DESIGN.md` § "The dispatcher does not grow new opcodes" applies with full force here.
