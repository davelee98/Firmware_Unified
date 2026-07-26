# `shared/core` + `shared/hal` API design

Concrete C interfaces for the target-agnostic core and the HAL it sits on. Grounded in the
four repos' implementations (see DIVERGENCE_MATRIX.md for the evidence and the resolutions
this design encodes). Everything here is **plain C, C99**, no C++ (`shared/` CI rejects
`.cpp`/`.hpp` and Arduino `String`), no vendor headers, no heap, no scheduler.

## The one hard constraint that shapes everything: the EFR32BG22

Every interface below must be satisfiable by the Silabs target, which is the floor:

- **No kernel.** Handlers run inline in the BGAPI event callback (`opendisplay_pipe.c:1271`),
  on the main stack. There is no thread to block, no `k_msleep`, no work queue.
- **~10.3 KB heap, ~21 KB static already committed** (`targets/efr32bg22-slc/README.md`). App
  code owns ~11.6 KB of statics today. `shared/core` must not *add* net RAM — it replaces the
  per-repo copies, and on BG22 it competes with a 3150-byte BT buffer pool for the heap.
- **No `malloc` in app code** (verified: zero `malloc`/`sl_malloc` in Silabs app sources). The
  core must run entirely on caller-provided or statically-sized buffers.
- **Handlers may not block.** The Silabs `wait_for_refresh` already blocks up to 60 s in the
  callback and drops the BLE link mid-refresh; that is a bug the shared model must not
  institutionalize. Long panel operations are driven by a **non-blocking pump**, not by
  blocking calls.

Consequences, stated once and applied throughout:

1. **No function-pointer HAL vtable is required** — targets are selected at *compile time*, so
   the HAL is a set of `extern` C functions the target links against (link-time binding), not a
   `struct` of pointers. This costs zero RAM and zero indirection, which the ESP32's
   two-existing-backends panel ladder already proves works. (A vtable is used in exactly one
   place — the panel ops — because a single target genuinely has 2-3 panel backends; see §3.)
   Link-time binding is also what makes the core **host-testable**: a test binary links stub
   HAL implementations and exercises every path without a board. Do not add a vtable "for
   testability" — that trade is already paid for. See ARCHITECTURE.md § "Everything on the wire
   is testable without hardware".
2. **The core owns all protocol state as file-static singletons**, sized by target macros.
   Single-connection is a given on all four repos (Silabs `MAX_CONNECTIONS=0`, others
   single-`connection`), so no per-connection context is needed.
3. **All parsing lives in the core; the HAL sees only bytes and geometry.** The NRF54 mistake
   of passing raw wire payloads into the panel driver (which then parses the 17-byte 0x76 BE
   header itself — `opendisplay_display.cpp:467-473`) is explicitly *not* reproduced.

## Feature parity across targets is **not** a goal

The BG22 constraint above bounds the *shape* of every interface — no heap, no blocking, no
kernel, caller-provided buffers. It does **not** bound the *feature set*. These are separate
axes and conflating them is the main way this design could go wrong:

- **Shape is universal.** Every signature must be callable from a bare-metal callback on a
  32 KB part. This is non-negotiable and applies to interfaces a given target will never call.
- **Coverage is per-target.** No target is required to implement every subsystem. This restates
  ARCHITECTURE.md's non-goal — *"Lowest-common-denominator features… capability differences are
  expressed through config and `#if`, not by removing features"* — at the level where the
  contract is actually written.

There is still a reference and a direction of travel: **`Firmware`'s ESP32 branch models the
feature set, and the other targets converge toward it** (DIVERGENCE_MATRIX.md § "Reference
implementation and direction of travel"). That governs *which shape to adopt* when two targets
do the same thing differently. It does **not** mean a target is behind because it lacks a
capability, and convergence is not one-way — NFC and Channel Sounding exist outside `Firmware`
and are not defects in it.

The divergence is real and permanent, not a migration backlog to burn down:

| Capability | Where it lives | Why it will not spread |
|---|---|---|
| PIPE sliding window `0x80`-`0x82` | ESP32 | 8.3 KB reorder queue; BG22 has ~10.3 KB of heap total |
| Partial region `0x76` | targets with a framebuffer | BG22 has no framebuffer to compose into |
| WiFi/LAN transport | ESP32 | no radio for it elsewhere |
| tinfl ROM inflate | ESP32 | ROM-provided; others use uzlib bit-serial |
| `CMD_NFC_ENDPOINT` (TNB132M) | EFR32BG22 | one product's hardware |
| Channel Sounding | nRF54L15 | silicon feature |
| Parallel e-paper (FastEPD) | ESP32-S3 | needs the S3 LCD peripheral + PSRAM |

So `shared/core` is a **superset with compile-time subsetting**, never an intersection. Promote
a subsystem into `shared/` when *one* target needs it and the logic is target-agnostic — not
when all four agree to adopt it. The mechanism is § "Capability gating" below; the rule this
section states is *when to reach for it*, which is: routinely, and without treating a gap as a
defect.

Two obligations follow, and both are easy to skip:

1. **A disabled subsystem still answers.** It NACKs its opcode rather than dropping the frame,
   so a host can tell "this device does not do that" apart from "this device is broken."
   Silence must never mean *unsupported* — a timeout is a transport failure and a host is right
   to retry it. This is the prerequisite for capability discovery's secondary mechanism
   (ARCHITECTURE.md § "Capabilities are discovered by interrogation, not assumed").
2. **Capability differences the host must know about become host-visible**, not silent. The
   `MAX_CONFIG_SIZE` split (2048 on BG22, 4096 elsewhere — DIVERGENCE_MATRIX) was the
   motivating example: a config that fit nRF was truncated on BG22 with no way for the sender
   to know. Divergence is fine; *undiscoverable* divergence is the bug.

   (That example was resolved on 2026-07-25 the other way — by making
   the value uniform at 4096 rather than making it discoverable, DIVERGENCE_MATRIX §2.7. It is
   kept here because the *principle* is unchanged: what could not be unified would still have
   to be discoverable.)

   Concretely, `od_config.c` must **clamp capability fields on write** (settled 2026-07-25 —
   an earlier draft of this paragraph said clamp on read, which was overturned: clamp-on-read
   makes config export lossy and collapses "can't" into "don't"; see ARCHITECTURE.md § "The
   gap, and a proposed fix" and DESIGN_REVIEW_2026-07-25.md § "Proposal 1"). Mask
   `transmission_modes` and `communication_modes` against a compile-time mask derived from
   this target's `OD_*_ENABLE` set at `CONFIG_WRITE`, apply the same mask at NVS load (so
   blobs provisioned by pre-clamp firmware are corrected too), and report "accepted with
   modifications" via a bit in the config ack's reserved bytes. `CONFIG_READ` stays a
   faithful, byte-stable mirror of stored config. Clamp only bits the firmware is
   authoritative over — never `partial_update_support`, `panel_ic_type`, geometry, or pins,
   which describe the attached panel the firmware cannot interrogate and must not overrule.
   That is the primary discovery mechanism and it costs no wire surface.

## Layering

```
target BLE/LAN glue ──frame in──► od_core_rx() ──enqueue──► od_core_process()
                                                                 │ parse, decrypt, dispatch
                                                                 ▼
                        ┌───────── shared/core ─────────────────────────────┐
                        │ dispatch · config TLV · xfer state machines ·     │
                        │ session/CCM · advert build · (PIPE, compile-opt)  │
                        └───────────────┬───────────────────────────────────┘
                          calls ▼                       ▲ target supplies
                 ┌── shared/hal (extern C) ──┐   ┌── shared/compress ──┐
                 │ time gpio spi i2c nvs      │   │ od_zlib_stream_*    │
                 │ crypto radio log panel     │   │ (uzlib, 512 B win)  │
                 └───────────────────────────┘   └─────────────────────┘
```

The core calls *out* to the HAL and *up* to the target only via `od_hal_radio_send` (response
egress). Nothing in `shared/` includes a vendor header.

---

## `shared/hal` — interface signatures

Each interface is a header of `extern` C declarations. The target provides the definitions in
`targets/<t>/hal/`. Return convention: `int` returning `0` on success, negative `od_err_t` on
failure, unless noted. No interface may block beyond a bounded busy-wait; anything long is
split into start + `busy()` poll (the panel model, §3).

### `od_hal_time` — already exists in embryo

Promote `Firmware_NRF54/src/nrf54_zephyr_compat.h` verbatim (already `od_`-prefixed):

```c
uint32_t od_hal_uptime_ms(void);          /* monotonic; wrap-safe by subtraction */
void     od_hal_delay_ms(uint32_t ms);    /* bounded busy/sleep; NOT a scheduler yield */
void     od_hal_delay_us(uint32_t us);
```

`od_hal_delay_ms` maps to `k_msleep` / `vTaskDelay` / `sl_sleeptimer_delay_millisecond`. The
core uses it **only** for short, bounded waits (e.g. the 20 ms TX-drain gap before a refresh,
`opendisplay_pipe.c:799`); it must never be used to wait out a panel refresh — that is the
pump's job (§3).

### `od_hal_gpio` — already exists in embryo

Promote `Firmware_NRF54/src/nrf54_gpio.h`. The critical portability decision: **`cfg` is an
opaque target-encoded pin byte**, decoded inside the HAL, never by the core. (nRF54 encodes
`(port<<4)|pin`; Silabs encodes `0xPN`; `opendisplay_structs.h` documents the field as
target-defined.) This is exactly why the same 66-panel config blob drives three different pin
encodings unchanged.

```c
#define OD_PIN_UNUSED 0xFFu
typedef void (*od_hal_gpio_irq_fn)(void);   /* ISR context: set a flag ONLY */

bool od_hal_gpio_decode(uint8_t cfg, uint8_t *port_out, uint8_t *pin_out);
void od_hal_gpio_config_output(uint8_t cfg, bool initial_high);
void od_hal_gpio_config_input(uint8_t cfg, bool pull_up, bool pull_down);
int  od_hal_gpio_config_irq(uint8_t cfg, od_hal_gpio_irq_fn handler);   /* edge-both */
void od_hal_gpio_write(uint8_t cfg, bool level_high);
int  od_hal_gpio_read(uint8_t cfg);
void od_hal_gpio_park(uint8_t cfg);        /* high-Z for deep sleep */
```

### `od_hal_spi`, `od_hal_i2c` — panel bus + sensors

The panel bus abstraction that already works cross-target is `bb_epaper`'s `*_io.inl` backends
(Silabs `silabs_efr32_io.inl`, ESP-IDF `esp_generic.inl`). Keep the panel SPI *inside*
`third_party/bb_epaper` (it can't satisfy the `shared/` rule) and expose only what the core's
non-panel code needs. I2C is used by sensors/PMIC, which are target drivers, so the core needs
almost nothing here — keep both minimal:

```c
/* SPI: panel bus is driven by bb_epaper's own backend. This is only for any
   core-level SPI need (none today) — declared for completeness, likely unused. */
int od_hal_spi_write(const uint8_t *data, uint32_t len);

/* I2C: sensor drivers live in targets/; the core does not call I2C directly. */
int od_hal_i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
int od_hal_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf, uint16_t len);
```

Note: `od_hal_i2c` should land on ESP-IDF's `driver/i2c_master.h` (IDF ≥ 5.2), never the
deprecated `driver/i2c.h` (TOOLCHAINS.md).

### `od_hal_nvs` — config blob persistence

The one storage semantic all three converge on. NRF54 = Zephyr `settings`, Silabs = NVM3,
ESP32 = LittleFS/NVS. Reduce to three calls (the NRF54 `opendisplay_config_storage.c` is the
model — three functions behind an already-clean seam):

```c
int  od_hal_nvs_load(uint8_t *buf, uint32_t cap, uint32_t *len_out);  /* 0 ok, -ENOENT empty */
int  od_hal_nvs_save(const uint8_t *buf, uint32_t len);
int  od_hal_nvs_erase(void);
```

The core owns the record framing (magic, version, inner CRC32 — which *is* enforced on load in
all three) so the HAL stores an opaque blob. `cap` carries `MAX_CONFIG_SIZE`, which since
2026-07-25 is **4096 on every target** (DIVERGENCE_MATRIX 2.7) — the parameter stays rather
than becoming a constant, because the HAL should not have to be recompiled to change a size the
core owns, and because it keeps the bound explicit at the call site. On BG22 this is the
4112-byte NVM3 record; see MEMORY_CONSTRAINTS.md item 3 for what that costs there.

### `od_hal_crypto` — the CCM decision

Firmware uses mbedTLS/CC310 **native CCM**; NRF54 and Silabs **hand-roll CCM over PSA AES-ECB**
(RFC 3610, `opendisplay_pipe.c:282-405`) because `PSA_ALG_CCM` isn't pulled in, at a cost of a
key import/destroy per 16-byte block. Right design: **expose CCM as the primitive** (every SDK
has native CCM — mbedTLS `mbedtls_ccm_*`, PSA `psa_aead_*`, CC3xx), and keep the RFC-3610-over-
ECB code as a *shared* soft fallback (`OD_CRYPTO_SOFT_CCM`) so a target lacking native CCM
selects it once instead of copying it. The five primitives the two `#ifdef` arms of
`encryption.cpp:31-42` already share are the exact seam:

```c
/* AEAD — one-shot. nonce is the 13-byte CCM nonce; aad is the 2 opcode bytes; tag 12 B. */
int  od_hal_ccm_encrypt(const uint8_t key[16], const uint8_t *nonce, uint8_t nonce_len,
                        const uint8_t *aad, uint8_t aad_len,
                        const uint8_t *plain, uint16_t plain_len,
                        uint8_t *cipher, uint8_t *tag, uint8_t tag_len);
int  od_hal_ccm_decrypt(const uint8_t key[16], const uint8_t *nonce, uint8_t nonce_len,
                        const uint8_t *aad, uint8_t aad_len,
                        const uint8_t *cipher, uint16_t cipher_len,
                        const uint8_t *tag, uint8_t tag_len, uint8_t *plain);
int  od_hal_cmac(const uint8_t key[16], const uint8_t *msg, uint32_t msg_len, uint8_t out[16]);
int  od_hal_aes_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);  /* KDF only */
int  od_hal_random(uint8_t *buf, uint16_t len);
```

`od_hal_aes_ecb` is needed by the KDF (`deriveSessionKey` finalizes with one ECB block); it is
*not* the CCM primitive in this design. `od_hal_cmac` handles auth challenge + session-id + PSK.
On Silabs/nRF, `od_hal_ccm_*` can be the shared soft implementation calling `od_hal_aes_ecb`
internally; on ESP32 it maps to native CCM. Verified API-identical PSA usage on nRF54 and Silabs
means one implementation covers both.

### `od_hal_radio` — transport-agnostic framed egress + origin

The core must reply on the *originating* transport only, and must know a frame's origin to
apply/skip the CCM envelope (SECTION 9 rule 4: TLS-PSK LAN bypasses app-layer CCM). Firmware
already does this with a `g_commandOrigin` and `frameOwnsSession()`
(`communication.cpp:27-45, 2115`). Make origin an explicit parameter, not a global:

```c
typedef enum { OD_ORIGIN_BLE = 0, OD_ORIGIN_LAN_PLAIN = 1, OD_ORIGIN_LAN_TLS = 2 } od_origin_t;

/* target implements: send a fully-framed response on the given transport. */
int  od_hal_radio_send(od_origin_t origin, const uint8_t *frame, uint16_t len);
bool od_hal_radio_notify_ready(od_origin_t origin);   /* CCCD enabled / socket up */
```

### `od_hal_log`

Firmware already funnels logging through `writeSerial(String,bool)` — same shape, wrong
signature/type. Silabs uses RTT, NRF54 RTT/UART, ESP32 `esp_log`. One line-sink:

```c
void od_hal_log(const char *line);   /* NUL-terminated, one line, no String */
```

**Every call site is bound by the no-secrets rule** (ARCHITECTURE.md § "Secrets are never
logged verbatim"): presence and length, never content — for SSID/password (`0x26`),
`security_config` (`0x27`), session and derived keys, nonces, MACs, and raw config/TLV
payloads. It applies at *all* levels, including `debug`, because debug logging is exactly what
gets enabled in the field. `od_config.c` is the highest-risk unit here: a parser that dumps its
input has logged a credential.

### `od_hal_panel` — the display-specific interface

This is the one interface worth real care, and `display_fastepd.h` (upstream) is the reference
shape. Its actual surface (verified):

```c
void opendisplay_fastepd_load_pins_from_display(const struct DisplayConfig*, const struct SystemConfig*, uint16_t panel_ic_type);
bool fastepd_init_failed(void);
void fastepd_prepare_hardware(void);   void fastepd_epaper_begin(void);
void fastepd_full_update(void);        bool fastepd_wait_refresh(int timeout_sec);
void fastepd_sleep_after_refresh(void);
void fastepd_direct_write_reset(void); void fastepd_direct_write_chunk(const uint8_t*, uint32_t);
void fastepd_direct_refresh(int mode /*0 FULL,1 FAST,2 PARTIAL*/); void fastepd_direct_sleep(void);
void fastepd_mark_hw_deinitialized(void);
void fastepd_partial_prepare(uint16_t x,uint16_t y,uint16_t w,uint16_t h);
bool fastepd_partial_write_chunk(const uint8_t*, uint32_t); bool fastepd_partial_refresh(int mode);
```

Five fixes turn it into a portable C panel HAL (gaps confirmed by the ESP32 analysis and by the
NRF54/Silabs `opendisplay_display.h` boundary):

1. **All C linkage.** `display_fastepd.h` wraps only 2 of 16 functions in `extern "C"`.
2. **A capability query**, so the core asks instead of scattering `getBitsPerPixel()` /
   `seeed_driver_used()` / `e1004_panel_used()` / `directWriteIsGray4()` across handlers — and
   so the core, not the panel driver, decides whether 0x76 is supported. Crucially this captures
   the **streaming vs framebuffer** split: bb_epaper streams to controller RAM with no
   framebuffer; FastEPD/Seeed buffer a full frame and blit. The core must not assume either.
3. **No blocking refresh in the call path.** Refresh is `start` + `busy()` poll, pumped (§ pump)
   — replacing both `fastepd_wait_refresh(timeout)` and Silabs' 60 s in-callback block.
4. **Symmetric error reporting.** `write_chunk` returns an int everywhere (FastEPD's full-frame
   path returns void and silently truncates; the partial path returns bool — pick one).
5. **Normalize plane order at the seam.** bb_epaper routes the first (old) plane to `PLANE_1`
   (`display_service.cpp:3207`); FastEPD routes plane 0 → `previousBuffer` (natural order). Left
   unnormalized, one backend inverts partial updates. The HAL contract fixes plane 0 = old.

Proposed interface. `od_pixfmt_t` and geometry are core types (from parsed config), never
vendor types. etag state stays in the **core** (it is protocol bookkeeping), not the panel:

```c
typedef enum { OD_PIX_1BPP=0, OD_PIX_2BPP, OD_PIX_4GRAY, OD_PIX_BWR, OD_PIX_BWY } od_pixfmt_t;
typedef enum { OD_REFRESH_FULL=0, OD_REFRESH_FAST=1, OD_REFRESH_PARTIAL=2 } od_refresh_t;

typedef struct {
    uint16_t width, height;
    od_pixfmt_t fmt;
    uint8_t  plane_count;         /* 1, or 2 for BWR/BWY/4gray */
    bool     needs_framebuffer;   /* false = streaming sink (bb_epaper), true = buffered (FastEPD) */
    bool     supports_partial;    /* 0x76 / PIPE-partial available on THIS panel */
} od_panel_caps_t;

int  od_hal_panel_init(const struct DisplayConfig *d, const struct SystemConfig *sys,
                       uint16_t panel_ic_type, od_panel_caps_t *caps_out);   /* <0 = init failed */
int  od_hal_panel_begin(void);                          /* full-frame session: open write window */
int  od_hal_panel_begin_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h); /* partial */
int  od_hal_panel_write(const uint8_t *bytes, uint32_t len);  /* sequential sink; plane 0 first */
int  od_hal_panel_refresh_start(od_refresh_t mode);     /* returns immediately */
bool od_hal_panel_refresh_busy(void);                   /* poll; false = done */
void od_hal_panel_sleep(void);
void od_hal_panel_abort(void);
void od_hal_panel_mark_deinitialized(void);             /* rail was cut behind our back */
```

Because a single target legitimately has 2-3 panel backends (ESP32: bb_epaper + FastEPD +
e1004; Silabs: bb_epaper only), this **one** interface is the place a function-pointer `struct
od_panel_ops *` earns its keep — selected once at `od_hal_panel_init` from `panel_ic_type` and
`display_technology`, exactly the predicate `fastepd_driver_used()` already computes. The
per-panel-controller quirks (EP397 Y-decrement, EP426 X-decrement, e1004 dual-CS) stay behind
the ops table, out of `shared/core`.

---

## `shared/compress` — inflate

One engine, already portable, already vendored identically in NRF54/Silabs and present in
Firmware: the resumable `od_zlib_stream.c` (byte-resumable rewrite of uzlib/tinf, pure C, no
vendor headers). Lift it under `shared/compress/` unchanged:

```c
void             od_zlib_stream_reset(uint32_t expected_output_size);
od_zlib_status_t od_zlib_stream_push(const uint8_t *input, size_t len, bool final);
od_zlib_status_t od_zlib_stream_poll(uint8_t *output, size_t cap, size_t *produced);
const char      *od_zlib_stream_error(void);
uint32_t         od_zlib_stream_output_count(void);
/* status: NEEDS_INPUT=0, OUTPUT_READY=1, DONE=2, ERROR=-1 */
```

Window size is a **compile-time macro** (`OPENDISPLAY_ZLIB_WINDOW_BITS`, floor 9 = 512 B) with
static (`USE_HEAP_WINDOW=0`) or heap allocation, exactly as today. The engine already rejects a
stream whose CMF declares a larger window than the target allows (`od_zlib_stream.c:641-644`) —
so this macro is a genuine wire contract, not just a buffer knob (see MEMORY_CONSTRAINTS.md).
The ESP32's ROM-tinfl adapter (`od_inflate_tinfl.*`) is a *build-level* alternative behind the
same four-symbol API (`#define` remap) — keep it optional and ESP32-only.

---

## `shared/core` — entry points

The whole command surface is driven by two functions plus init. This shape is dictated by the
Silabs no-kernel constraint: RX enqueues, a pump drains — never blocking the caller.

```c
/* one-time init: wire the parsed config in, init crypto, reset state. */
void od_core_init(void);

/* called from the transport's RX callback (BT thread / BGAPI callback / LAN task).
   Copies the frame into an internal ring and returns immediately — NO parsing,
   NO crypto, NO panel I/O here. `origin` selects the reply transport + CCM policy. */
void od_core_rx(od_origin_t origin, const uint8_t *frame, uint16_t len);

/* called from the main loop / superloop. Drains queued frames: parse → (decrypt) →
   dispatch → reply via od_hal_radio_send. Also advances any in-flight panel refresh
   via od_hal_panel_refresh_busy(). Returns quickly; never blocks on a refresh. */
void od_core_process(void);
```

On Silabs the ring depth is 1 and `od_core_process` is called right after the event handler
(same superloop pass); on NRF54 the ring is the `K_MSGQ` (depth 8) drained on the main thread;
on ESP32 it is the existing `commandQueue`. The **ring is target-sized** (a `OD_RX_QUEUE_DEPTH`
macro) but the logic is one implementation.

### Internal structure (one file per subsystem, all file-static state)

| Unit | Responsibility | Primary donor |
|---|---|---|
| `od_dispatch.c` | opcode BE parse, encryption gate, the `CMD_*` switch, response framing helpers (2/4-byte acks, NACK namespaces) | NRF54 `dispatch()` (structure), Firmware (coverage) |
| `od_config.c` | TLV walk into `opendisplay_structs.h` types, chunked assembly, CRC, `od_hal_nvs` calls | NRF54 size-table parser (unknown-TLV safety) |
| `od_xfer_direct.c` | 0x70/0x71/0x72 state machine, streaming into `od_hal_panel_write`, inflate integration | Firmware (most complete) |
| `od_xfer_partial.c` | 0x76 header parse (17-byte BE), etag policy, two-plane routing | Firmware / NRF54 (parity) |
| `od_pipe.c` | 0x80-0x82 sliding window + reorder + SACK — **compile-gated `OD_PIPE_ENABLE`** | Firmware (only impl) |
| `od_session.c` | auth handshake, KDF, nonce/replay, CCM envelope over `od_hal_ccm_*` | any (byte-identical across all four) |
| `od_advert.c` | 16-byte MSD build (sensor encode, voltage/temp clamp, status bits) | any (identical) |

### The dispatcher does not grow new opcodes

`od_dispatch.c` implements the opcode set the canonical header already defines. **Promoting
logic into `shared/core` is not an occasion to add wire surface** — it is the moment the
temptation is strongest, because the refactor exposes every awkward corner at once, and a new
opcode looks like a cheap way to tidy one.

Reach for the cheaper options first — an existing field, a reserved/extension field, a new
error code in an existing namespace, or a config TLV packet type (versioned and skippable, so
old firmware degrades gracefully). Only then a new opcode.

**If a new opcode would genuinely simplify the structure, propose it** — with what it replaces,
why the cheaper options cannot carry it, and its cost on targets that will only ever NACK it.
The rule is *argue for it in the docs*, not *avoid it*. DIVERGENCE_MATRIX.md § "The opcode
space is conservative" holds the full ordering and two live candidates (capability/limits
discovery for the `MAX_CONFIG_SIZE` split; an explicit compressed-payload flag in place of the
current length inference). Adding one is blocked mechanically anyway while the canonical
headers are frozen — which is the right default for a decision this permanent.

### Capability gating — a target never pays for what it lacks

This is the mechanism for § "Feature parity across targets is **not** a goal"; read that first
for when to reach for it.

Config, dispatch, direct-write, session, and advert are unconditional (all targets implement
them). The optional subsystems are compile-time flags the target sets, and a disabled subsystem
**still NACKs its opcode** rather than dropping it silently (the Silabs fail-fast NACK for 0x76
is the correct model; its silent drop of 0x45/0x80-0x82 is not — DIVERGENCE_MATRIX §1):

```c
#define OD_PIPE_ENABLE      0   /* Silabs/NRF54: 0 — no 8.3 KB reorder queue in .bss */
#define OD_PARTIAL_ENABLE   0   /* Silabs: 0 (no framebuffer) */
#define OD_NFC_ENABLE       1   /* Silabs/NRF54: 1; Firmware: 0 */
#define OD_BUZZER_ENABLE    0   /* Silabs: 0 */
#define OD_LAN_ENABLE       0   /* ESP32-with-WiFi only */
```

`OD_PIPE_ENABLE=0` must exclude the reorder queue from `.bss` entirely (`#if` around the
`PipeReorderSlot[]`), because 8.3 KB — or even the 4.3 KB small-window form — does not fit BG22.

### Invariants the core must assert at compile time

Two numeric couplings are load-bearing and undocumented outside code comments:

```c
/* PIPE window is bounded by the AES-CCM replay window: in-flight ≤ W ≤ 32 ≤ ±32.
   Raising PIPE_MAX_W without raising the replay window silently breaks decryption
   under packet reorder (communication.cpp:740-745 ↔ encryption.cpp:128-136). */
_Static_assert(OD_PIPE_MAX_W <= OD_REPLAY_WINDOW_HALF, "PIPE window exceeds replay window");

/* reorder slot indexing seq % SLOTS is collision-free only if a live window fits. */
_Static_assert(OD_PIPE_REORDER_SLOTS == OD_PIPE_MAX_W + 1, "slot count must be W+1");
```

### Bugs to fix on promotion (not port verbatim) — from DIVERGENCE_MATRIX

- Config TLV: add a real per-record length, or at minimum keep NRF54's size-table so one
  unknown packet type does not abandon the rest of the config (Firmware/Silabs both do).
- Silabs: gate encryption on `sec_enabled()`, not `frame_len ≥ 31` (§1.5a security hole).
- Silabs: clear the session after a config save (key may have changed — §2.4).
- Session timeout: absolute from `session_start`, not idle (§6.2); reject exact-counter replay
  (`diff==0` currently accepted on NRF54/Silabs).
- Replay window is a 64-entry `uint64_t` linear-scan list (512 B) for a ±32 accept window —
  8× oversized; a 64-bit bitmap over the window is ~8 bytes and BG22 wants that RAM back.
