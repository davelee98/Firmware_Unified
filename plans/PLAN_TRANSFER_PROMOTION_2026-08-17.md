# Transfer-plane promotion plan

**Date:** 2026-08-17

**Source snapshot:** `main` at `b95640b`

**Status:** proposed execution plan, derived from the current production sources, canonical wire
headers, the PIPE wire description, and the current Python client. This document is standalone.

## 1. Objective

Move image-transfer and NFC protocol policy out of the three target trees and into portable,
host-tested C under `shared/`, while retaining target ownership of panel drivers, buses, power,
refresh waiting, NFC controllers, transport delivery, and platform recovery.

Completion means:

- one shared direct-write state machine for `0x70`/`0x71`/`0x72`;
- one shared partial-write state machine for `0x76` plus its `0x71`/`0x72` continuation;
- one shared PIPE state machine for `0x80`/`0x81`/`0x82`, compiled out where unsupported;
- one shared NFC endpoint state machine for `0x83`, compiled out where unsupported;
- one shared streaming-inflate API and one shared pump;
- one shared geometry and byte-count implementation;
- no target-owned wire parsing, SACK construction, transfer ownership, byte accounting, etag
  policy, compression-finalization policy, or NFC chunk assembly;
- unsupported targets retain their exact defined wire behavior without allocating disabled state;
- every capable silicon/backend/transport row passes hardware verification; and
- the old target state and command hooks are deleted in the same promotion series that replaces
  them.

This is not a panel-driver rewrite. It does not unify SPI, GPIO, BUSY polarity, panel initialization
sequences, controller address-window quirks, FastEPD buffering, NFC IC commands, or boot-screen
rendering.

## 2. Ground truth examined

### 2.1 Wire definitions

The fixed protocol surface comes from:

- [`shared/protocol/opendisplay_protocol.h`](../shared/protocol/opendisplay_protocol.h): universal
  framing, opcodes, reply shapes, error namespaces, encrypted envelope, BLE limits, and LAN rules;
- [`shared/protocol/opendisplay_structs.h`](../shared/protocol/opendisplay_structs.h): asserted
  layouts for `PipeStartRequest`, `PipePartialExt`, `PipeStartResponse`, `PipeSack`, and
  `PartialWriteStartHeader`; and
- [`../Firmware/docs/pipe-write-protocol.md`](../../Firmware/docs/pipe-write-protocol.md): the
  end-to-end PIPE sequence, SACK interpretation, reorder behavior, auto-completion, partial mode,
  and fallback behavior.

The current client behavior was checked in:

- [`py-opendisplay/device.py`](../../py-opendisplay/src/opendisplay/device.py), especially direct,
  partial, PIPE, and NFC upload flows;
- [`py-opendisplay/protocol/commands.py`](../../py-opendisplay/src/opendisplay/protocol/commands.py);
- [`py-opendisplay/protocol/responses.py`](../../py-opendisplay/src/opendisplay/protocol/responses.py);
- [`py-opendisplay/encoding/compression.py`](../../py-opendisplay/src/opendisplay/encoding/compression.py);
- [`py-opendisplay/encoding/bitplanes.py`](../../py-opendisplay/src/opendisplay/encoding/bitplanes.py);
  and
- the corresponding `test_pipe_write.py`, `test_pipe_write_sender.py`, partial, and NFC unit tests.

### 2.2 Production implementation inventory

| Area | ESP32-IDF | Nordic/Zephyr | EFR32BG22 |
|---|---|---|---|
| Direct/partial/full transfer | `src/display_service.cpp` | `src/opendisplay_display.cpp`, `src/od_cmd_direct.c` | `opendisplay_display.cpp`, `od_cmd_silabs.c` |
| PIPE | `src/display_service.cpp` | `src/opendisplay_pipe_write.cpp` | unsupported hooks in `od_cmd_silabs.c` |
| NFC endpoint | silent `UNKNOWN` hook in `src/od_cmd_app.cpp` | `src/od_cmd_nfc.c` | `od_cmd_silabs.c` |
| NFC hardware | none | `src/opendisplay_nfc.c` | `opendisplay_ble.c` TNB132M routines |
| Inflate engine | uzlib or target tinfl adapter | uzlib | uzlib |
| Inflate pump | two copies in `display_service.cpp` | two copies in `opendisplay_display.cpp` | one copy in `opendisplay_display.cpp` |
| Geometry/format math | inline in `display_service.cpp` | `opendisplay_display_color.c` | divergent `opendisplay_display_color.c` |
| Panel abstraction candidate | uncalled `hal/od_hal_panel.*` plus two backend tables | none | none |

Current source volume in the main transfer-bearing files is:

| File | Lines |
|---|---:|
| `targets/esp32-idf/src/display_service.cpp` | 3,305 |
| `targets/nordic-zephyr/src/opendisplay_display.cpp` | 1,332 |
| `targets/nordic-zephyr/src/opendisplay_pipe_write.cpp` | 595 |
| `targets/nordic-zephyr/src/od_cmd_nfc.c` | 206 |
| `targets/efr32bg22-slc/opendisplay_display.cpp` | 921 |
| `targets/efr32bg22-slc/od_cmd_silabs.c` | 443 |
| portable uzlib stream implementation | 723 |
| ESP32 tinfl adapter | 203 |

These files contain unrelated display and command code too; the numbers are scope indicators, not
a promised deletion total.

### 2.3 Actual capability matrix

The implementation, not stale target annotations, gives this matrix:

| Capability | ESP32-IDF | Nordic/Zephyr | EFR32BG22 |
|---|---:|---:|---:|
| Direct write | yes | yes | yes |
| Streaming zlib inflate | yes | yes | yes |
| Legacy partial `0x76` | yes | yes | no |
| PIPE full-frame | yes | yes | no |
| PIPE partial | yes | yes | no |
| NFC endpoint | no | yes | yes |
| BLE | yes | yes | yes |
| LAN direct write | yes | no | no |
| LAN PIPE | deliberately refused | n/a | n/a |

Nordic has a 32-frame PIPE window. ESP32 normally has 32 and uses 16 on the classic ESP32 N4
profile. BG22 defines `OD_CAP_PIPE=0` and `OD_CAP_PARTIAL=0`. BG22 has 32 KB total RAM and the
current linked image reports only 480 bytes of heap-inclusive data/BSS headroom, so a disabled
feature must contribute zero state there.

### 2.4 Current test coverage

[`tests/host/pipe_write_test.c`](../tests/host/pipe_write_test.c) directly compiles the Nordic PIPE
state machine and checks basic START failures, cadence ACKs, a gap, fatal errors, auto-completion,
END behavior, and reply-substitution cleanup. It does not cover compressed PIPE, partial PIPE,
sequence wrap, negotiated frame enforcement, the reduced window, owner identity, or target parity.

There is no host test that owns the production direct or partial state machine. The only standalone
inflate harness is `targets/esp32-idf/tools/test_zlib_stream.c`, and it is not part of the host test
suite. Silabs fault tests exercise command integration and END ordering but do not make the transfer
state itself shared or exhaustively test it.

## 3. Wire contract to preserve

### 3.1 Common framing and confidentiality

- Requests begin with a two-byte big-endian opcode.
- Replies are `[status][low opcode byte][data...]`.
- Application ACKs are sealed when a security session is active.
- Hard NACKs are plaintext.
- A reply failure is part of the handler result. A handler must not return `OD_CMD_OK`, refresh a
  panel, or emit a later success after `od_reply()` substituted or refused an earlier ACK.
- A transfer is owned by the full immutable reply identity `{origin, tag}`, not merely BLE versus
  LAN and not a reusable connection handle.

### 3.2 Direct write: `0x70`, `0x71`, `0x72`

- An uncompressed START has no body.
- A body of at least four bytes selects compression: four-byte little-endian decompressed size,
  followed by optional first zlib bytes.
- DATA carries raw image bytes or the continuing zlib stream.
- END carries a refresh selector and optionally a four-byte big-endian new etag.
- END success is ordered: `00 72`, then the physical refresh, then `00 73` or `00 74`.
- The END ACK must be accepted by the peer-facing transport before the blocking refresh begins.
- A new START aborts any prior direct, partial, or PIPE transfer.

ESP32 currently auto-completes an uncompressed direct transfer when its byte count is reached;
Nordic and BG22 ACK the final DATA as `00 71` and wait for explicit END. The current Python client
supports both. Promotion must preserve this per-fleet compatibility initially through one
compile-time profile value, not two code paths:

```c
#define OD_XFER_DIRECT_AUTO_END 0 /* Nordic and BG22 */
#define OD_XFER_DIRECT_AUTO_END 1 /* ESP32 */
```

The knob is data consumed by the same shared state machine. Removing it later is a separate wire
compatibility decision supported by client-fleet evidence, not part of source movement.

### 3.3 Partial write: `0x76` continued by `0x71` and `0x72`

- The 17-byte START header is entirely big-endian except its one-byte flags field.
- Only compressed bit 0 is accepted.
- `old_etag` must be nonzero and equal the displayed etag; `new_etag` must be nonzero.
- The panel must support partial update and use the two-1bpp-plane representation.
- Rectangle width and height must be nonzero, the rectangle must fit without integer overflow,
  and `x` and width must be multiples of eight.
- Expected bytes are `2 * ceil(width / 8) * height`, with row padding.
- Any geometry, stream, incompleteness, or refresh failure clears the displayed etag.
- A successful refresh commits the new etag only after the panel reports success.
- A partial transfer never auto-completes; END is required.

### 3.4 PIPE: `0x80`, `0x81`, `0x82`

- START body is the asserted ten-byte little-endian `PipeStartRequest` and, for partial mode, the
  asserted twelve-byte little-endian `PipePartialExt`.
- The PIPE partial START `old_etag` is little-endian; the END `new_etag` remains big-endian.
- START replies report device maxima. Both peers independently apply the min rule to obtain the
  effective window, ACK cadence, and frame.
- Window and cadence floor at one, cadence cannot exceed window, and the window cannot exceed the
  32-bit SACK reach.
- DATA sequence numbers wrap modulo 256. In-order bytes reach the sink; ahead-within-window bytes
  wait in the reorder queue; duplicates are discarded and re-ACKed; frames outside the window in
  both directions are fatal.
- A SACK is `[00][81][highest_seen][mask:4 LE]`; `highest_seen` is implicitly ACKed and bit `i`
  covers `highest_seen - 1 - i`.
- A DATA NACK is fatal. Subsequent DATA for a dead or fatal transfer is silently refused.
- END first emits a tail SACK, then validates completeness, then emits `00 82`, refreshes, and
  reports `00 73` or `00 74`.
- Only uncompressed full-frame PIPE may auto-complete. Compressed and partial PIPE require END.
- PIPE is forbidden on LAN; the shared hook can enforce this directly from `ctx->rp.origin`.

### 3.5 NFC: `0x83`

- READ returns `[00][83][80][rec_type][len:2 BE][data]`.
- Single WRITE is `[01][rec_type][len:2 BE][data]` and returns status `81`.
- Chunked WRITE uses START `10`, DATA `11`, END `12`; START and DATA return status `82`, END returns
  `81`.
- The chunk buffer is exactly 512 bytes.
- Chunk state is bound to `{origin, tag}` and reset on that owner's disconnect.
- NFC NACK is always `[FF][83][FF][NFC_ERR_*]`.
- A read exposes at most `OD_SESSION_PAYLOAD_MAX - 4`, currently 218 tag bytes, in both encrypted
  and plaintext sessions so the advertised limit does not change with authentication state.
- ESP32 remains silent and returns `OD_CMD_UNKNOWN`; it must not allocate the read or write buffers.

### 3.6 Compression

- The host emits a zlib wrapper around DEFLATE and pins `window_bits=9`.
- Every firmware backend must reject a CMF declaring a window above nine bits.
- The declared decompressed total is exact. DONE with fewer or more sink bytes is failure.
- Truncation, checksum failure, invalid Huffman data, back-reference before output, sink refusal,
  and finalization requiring more input are failures.
- Engine selection is target/build policy; pump behavior and failure mapping are shared.

## 4. Source discrepancies to settle before moving handlers

These are source/protocol mismatches discovered during the inventory. They must be resolved in the
wire authority and pinned by tests before a shared handler becomes the only implementation.

### 4.1 Correct capability annotations

The canonical header still describes PIPE as ESP32-only even though Nordic builds and routes
`opendisplay_pipe_write.cpp`. It also contains historical target naming that does not match the
unified target layout. Update the canonical source and sync the generated copies. Do not edit only
the vendored header in this repository.

### 4.2 Name what the PIPE START response actually carries

Both firmware implementations send `PIPE_MAX_W`, `PIPE_MAX_N`, and `PIPE_MAX_FRAME`; the Python
client then applies the min rule. The response therefore contains device maxima, not the already
effective values. Correct any struct prose saying that the device echoes negotiated effective
values. Keep the bytes unchanged.

### 4.3 Make error `0x02` unambiguous

Firmware uses START error `0x02` for unsupported flag bits. The Python client also treats `0x02`
as “compressed mode unavailable” and retries without compression. Define it as
`OD_ERR_PIPE_START_UNSUPPORTED_FLAG`: it covers an unknown bit and a recognized optional flag that
this runtime profile cannot honor. Do not introduce a second raw meaning at call sites.

### 4.4 Standardize PIPE-unavailable `0x04`

BG22 currently sends `FF 80 04 00`, while the canonical namespace calls `0x04` unused and the
Python client calls it busy/bad state. Assign one canonical meaning:

```text
OD_ERR_PIPE_START_UNAVAILABLE = 0x04
```

It covers a target compiled without PIPE, a transport on which PIPE is prohibited, or a transient
state in which the device cannot open a PIPE session. DATA and END on a target without PIPE remain
silent, preventing a fatal `0x81` NACK from disrupting an unrelated upload.

### 4.5 Add missing names without changing bytes

Add canonical names for the `0x76`, `0x80`, `0x81`, and `0x82` echo bytes, PIPE response flags,
PIPE DATA errors, and the partial compressed flag. Raw literals are currently repeated across two
state machines and the client. This is a symbol-only protocol cleanup.

### 4.6 Define exact length policy

Pin the permissive behavior the sources and clients rely on:

- direct START lengths 0..3 are uncompressed; bytes 1..3 are tolerated and ignored;
- direct/PIPE END uses byte 0 when present and recognizes a new etag only when at least five bytes
  are present; bytes after byte 4 are ignored for forward compatibility;
- legacy partial END accepts at most its refresh byte because its etag was fixed at START;
- PIPE START accepts trailing bytes after the required 10 or 22 bytes;
- NFC single WRITE accepts declared data plus trailing extension bytes but writes only the declared
  data; and
- zero-length NFC DATA is malformed.

Encode these choices in corpus vectors so a later bounds cleanup cannot silently change them.

## 5. Correctness tightening included in promotion

The following changes close implementation gaps without changing a valid client's successful byte
sequence.

### 5.1 Enforce negotiated PIPE frame size

Both PIPE implementations store `frame_eff` but DATA only checks against a 248-byte local slot.
Shared PIPE must reject when:

```text
body_length + 2-byte opcode > frame_eff
```

and must reject a START whose effective frame cannot hold opcode, sequence, and at least one data
byte. Slot data width becomes `PIPE_MAX_FRAME - PIPE_FRAME_OVERHEAD` (241), not 248. That saves 231
bytes at 33 slots and makes the allocation derive from the wire ceiling.

### 5.2 Bind every multi-frame operation to the full owner

ESP32 records only origin for image transfers; Nordic and BG22 rely on one live BLE connection and
disconnect cleanup. Shared state stores `{origin, tag}` at START and silently refuses DATA/END from
another identity. NFC chunk state uses the same rule. A recycled connection handle cannot inherit
an old transfer.

### 5.3 Reject unavailable compression before accepting START

The runtime transmission-mode bit and compiled backend must both allow compression. Direct START
returns its existing `FF 70` failure. PIPE START returns
`OD_ERR_PIPE_START_UNSUPPORTED_FLAG`. Do not ACK PIPE and then leave `error=true` for the first DATA
to discover silently.

### 5.4 Enforce arithmetic bounds before hardware access

All width, height, row-byte, plane-byte, double-plane, received-plus-chunk, and rectangle-end
arithmetic runs in a checked 64-bit intermediate. A value that cannot fit the 32-bit state or panel
contract is rejected before panel power or buffer writes.

### 5.5 Propagate every reply result

START, DATA, SACK, END, refresh status, and NFC ACK sites must inspect their `od_txq_status_t`.
After a substituted/refused reply:

- no later success is emitted;
- no refresh begins if the END ACK failed;
- the transfer is aborted and its reservation released;
- PIPE is reset after a fatal reply failure; and
- NFC chunk state is reset if the host was told the stage failed.

### 5.6 Add one shared transfer timeout

ESP32 has a 15-minute watchdog; Nordic and BG22 have no equivalent transfer-state deadline. Shared
state records START time and exposes `od_xfer_service(now_ms)`. At 900,000 ms it aborts panel state,
clears partial etag state when required, drops PIPE reorder state, and returns to IDLE. The target
calls the service from its existing bounded main/superloop pump. No timer callback may mutate the
state.

### 5.7 Preserve ACK-before-refresh as a hard gate

Add a target seam:

```c
bool od_xfer_app_end_ack_on_air(const od_reply_t *owner, uint32_t deadline_ms);
```

The shared END path queues the ACK, calls this seam, and refreshes only on true. Implementations may
use exact controller TX-resource reporting or the strongest completion primitive their stack
offers. API unavailability is not negotiation failure: it invokes the target's recovery policy
(BG22 enters its bootloader path) rather than hanging or refreshing after an unprovable ACK.
Ordinary MTU negotiation does not call this recovery path.

Hardware sniffer evidence remains mandatory because stack acceptance alone does not prove that the
packet preceded the panel's blocking interval.

## 6. Resulting architecture

### 6.1 Shared modules

Add these production modules:

```text
shared/core/od_xfer_geometry.[ch]   format, row and plane byte math
shared/compress/od_inflate.h        one backend-neutral streaming API
shared/compress/od_inflate_pump.c   push/poll/finalize into the transfer sink
shared/core/od_xfer.[ch]            owner, arbitration, reset, timeout, common stream state
shared/core/od_xfer_direct.c        0x70/0x71/0x72 and refresh finalization
shared/core/od_xfer_partial.c       0x76 validation and two-plane region state
shared/core/od_pipe.c               0x80/0x81/0x82 negotiation, reorder and SACK
shared/core/od_nfc_cmd.c            0x83 parsing and 512-byte chunk assembly
shared/core/od_xfer_app.h           target lifecycle and displayed-etag seam
shared/hal/od_hal_panel.h           panel streaming/refresh boundary
shared/hal/od_hal_nfc.h             tag read/write boundary
```

`shared/sources.cmake` gains explicit tiers for the panel and NFC seams. Disabled capability
sources may compile their tiny hook behavior, but large contexts and buffers are under compile-time
guards and disappear from the object.

### 6.2 Common transfer state

One state object owns all panel transfer modes:

```c
enum od_xfer_mode {
    OD_XFER_IDLE,
    OD_XFER_DIRECT_FULL,
    OD_XFER_DIRECT_PARTIAL,
    OD_XFER_PIPE_FULL,
    OD_XFER_PIPE_PARTIAL,
    OD_XFER_FATAL
};

struct od_xfer_state {
    enum od_xfer_mode mode;
    od_reply_t owner;
    uint32_t started_ms;
    uint32_t expected_bytes;
    uint32_t received_bytes;
    uint32_t written_bytes;
    bool compressed;
    struct od_xfer_partial_state partial; /* present only with OD_CAP_PARTIAL */
};
```

There are not separate `directActive`, `partial.active`, and `pipe.active` booleans. An enum makes
impossible combinations unrepresentable. PIPE contains its sequencing context and feeds the same
full or partial sink; it does not own a second byte counter or decompressor.

### 6.3 Panel HAL boundary

The shared core needs this behavior, not vendor objects:

```c
struct od_panel_caps {
    uint16_t width;
    uint16_t height;
    uint8_t color_scheme;
    uint8_t plane_count;
    bool supports_partial;
};

int  od_hal_panel_caps(struct od_panel_caps *out);
int  od_hal_panel_begin_full(void);
int  od_hal_panel_begin_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               uint32_t plane_bytes);
int  od_hal_panel_write(const uint8_t *data, uint32_t len);
int  od_hal_panel_refresh(uint8_t mode, uint32_t timeout_ms, bool *completed);
void od_hal_panel_abort(void);
```

The first promotion retains synchronous refresh waiting because all three production paths are
synchronous today and their watchdog/stack scheduling differs. Nonblocking refresh is valuable but
would also require deferred encrypted completion replies and a new session-lifetime rule; combining
that redesign with state-machine promotion would prevent behavioral bisecting.

The target panel implementation owns:

- power rail and settle delays;
- panel/controller selection and init sequences;
- SPI/parallel bus setup and teardown;
- controller-specific address windows and decrementing axes;
- controller plane mapping and the sequential plane switch;
- FastEPD framebuffer versus bb_epaper streaming behavior;
- BUSY polarity, refresh polling, watchdog feeding, and sleep;
- touch suspend/resume and power keep-alive; and
- abort-to-known-hardware-state behavior.

The shared core owns the expected byte count, overflow refusal, compressed stream, transfer mode,
etag rules, and reply sequence.

The existing ESP32 `od_hal_panel` files are useful evidence for the vocabulary, but they currently
have no production caller. Do not promote them verbatim and declare the seam proven. First bind
them to production ESP32 behavior, then implement the same tested contract on Nordic and BG22.

### 6.4 Inflate boundary

All backends implement:

```c
void od_inflate_reset(uint32_t expected_output);
enum od_inflate_status od_inflate_push(od_span_t input, bool final);
enum od_inflate_status od_inflate_poll(uint8_t *out, uint16_t cap, uint16_t *used);
const char *od_inflate_error(void);
```

The shared pump owns the loop and calls `od_hal_panel_write()`. The portable uzlib implementation
moves to `shared/compress/`; the ESP32 ROM/tinfl implementation remains a selectable backend behind
the same symbols. Exactly one backend is linked. Both enforce the nine-bit wire window.

Use a target-set `OD_INFLATE_OUTPUT_CHUNK` with a 256-byte default. This is a compile-time storage
profile, not target code. BG22 and Nordic keep 256; ESP32 may retain 2,048 only if measured
throughput justifies its 1,792-byte cost.

No heap-backed 512-byte history window is permitted. A static history window gives deterministic
failure behavior and removes the current ESP32 boot-lifetime allocation failure mode.

### 6.5 NFC HAL boundary

Rename the existing identical target-facing operations into a real HAL:

```c
bool od_hal_nfc_read(uint8_t *type, uint8_t *data, uint16_t *len_io, uint16_t cap);
bool od_hal_nfc_write(uint8_t type, const uint8_t *data, uint16_t len);
```

NDEF encoding, controller I/O, NFCT emulation, TNB132M register access, field detect, and
advertisement updates remain target code. The command parser, record-type validation, response
framing, 218-byte read limit, 512-byte assembler, owner binding, and reset are shared.

### 6.6 Compile-time capability surface

Extend the existing capability header with validated boolean or bounded integer definitions:

```text
OD_CAP_ZLIB
OD_CAP_PARTIAL
OD_CAP_PIPE
OD_CAP_NFC
OD_PIPE_MAX_W
OD_PIPE_MAX_N
OD_XFER_DIRECT_AUTO_END
OD_INFLATE_OUTPUT_CHUNK
```

Required compile-time assertions include:

- `1 <= OD_PIPE_MAX_W <= PIPE_ACK_MASK_BITS` when PIPE is enabled;
- `1 <= OD_PIPE_MAX_N <= OD_PIPE_MAX_W`;
- reorder slots equal `OD_PIPE_MAX_W + 1`;
- RX and TX queue usable capacity covers a full PIPE window plus END;
- PIPE slot data equals `PIPE_MAX_FRAME - PIPE_FRAME_OVERHEAD`;
- BG22 has zero reorder, partial, and NFC-disabled allocations as applicable;
- inflate output chunk is nonzero and fits the panel write length type; and
- wire structs retain their asserted sizes.

## 7. Execution sequence

Every phase below is an independently revertible commit series. A phase is not complete while its
old production implementation remains callable.

### Phase 0 — Freeze observable behavior

1. Correct and sync the protocol constants and prose identified in section 4.
2. Add wire vectors for every request, ACK, NACK, and permissive-length boundary.
3. Add trace capture harnesses around the current production hooks with fake panel/NFC drivers.
4. Capture target-specific direct auto-END behavior explicitly.
5. Record baseline `.text`, `.data`, `.bss`, heap-inclusive BG22 size, stack high-water where
   available, transfer throughput, and refresh timing.
6. Build every existing target before any source moves.

Exit gate: a test failure can identify a wire byte, verdict, state transition, or hardware call
ordering rather than merely report that an upload failed.

### Phase 1 — Promote geometry and format math

1. Implement checked row-padded geometry in `od_xfer_geometry`.
2. Cover every canonical color scheme and widths around byte boundaries: 1, 7, 8, 9, 121, 122,
   127, 128, and configured production widths.
3. Prove BWR, BWY, and GRAY4 use two row-padded 1bpp planes.
4. Prove BWRY uses row-padded 2bpp and BWGBRY, GRAY16, SEVEN_COLOR, and BWGBRY_SPLIT use their
   actual client encoding width.
5. Cross-check expected byte counts against Python encoders.
6. Replace ESP32 inline math and both target `opendisplay_display_color.c` copies.

This phase must correct BG22's current flat-pixel calculation and GRAY4 classification before
shared direct-write trusts its totals. Test the 122-pixel-wide case explicitly; flat and row-padded
math differ there.

Exit gate: all targets call one geometry implementation and no target computes expected transfer
bytes independently.

### Phase 2 — Promote compression engine and pump

1. Move the portable stream implementation and header under `shared/compress`.
2. Delete the duplicate ESP32 uzlib source copy and unused checksum translation units.
3. Remove heap-window support.
4. Put the push/poll/sink loop in `od_inflate_pump.c`.
5. Adapt tinfl to the same backend symbols and enforce the nine-bit CMF limit.
6. Add the portable and tinfl differential suites to CTest.
7. Replace all five target pump loops.

Required tests:

- complete streams delivered in every split from one byte through frame-sized chunks;
- back-references crossing input and output chunk boundaries;
- exact expected output, one byte short, one byte long, truncated trailer, bad Adler-32, invalid
  CMF/FLG, preset dictionary, and window bits 10..15;
- final called twice, reset after every failure, sink refusal, and integer overflow; and
- byte-identical output/status progression between uzlib and tinfl for the shared corpus.

Exit gate: one pump exists, exactly one backend links per image, and all targets preserve their
current successful compressed upload.

### Phase 3 — Promote NFC independently

NFC does not depend on panel transfer and provides a small, stateful proof of the ownership/reset
pattern before the larger image state moves.

1. Add `od_hal_nfc.h` and rename the Nordic/BG22 driver entry points.
2. Implement `od_nfc_cmd.c` with compile-out support.
3. Store owner `{origin, tag}` on chunk START.
4. Reset on owner disconnect, a replacement START, overflow, tag-write failure, reply
   substitution, and core reset. Preserve the retryable short-END behavior unless the wire corpus
   explicitly changes it.
5. Route the shared `od_cmd_app_nfc()` hook on all targets.
6. Delete Nordic `od_cmd_nfc.c` and the NFC parser/buffers from `od_cmd_silabs.c`.
7. Confirm ESP32 produces `OD_CMD_UNKNOWN`, no reply, no activity stamp, and no NFC buffers.

Required tests cover every subcommand; lengths 0, 1, 120, 121, 218, 219, 512, and 513; all five
record types; each NFC error; wrong-owner DATA/END; disconnect/reconnect with a recycled handle;
read and write driver failure; reply failure; and encrypted/plain read-size equality.

Exit gate: the only target NFC command code is HAL adaptation; BG22 static RAM does not increase.

### Phase 4 — Establish panel and transfer lifecycle seams

1. Move the panel contract to `shared/hal` after adapting it to the API in section 6.3.
2. Implement production backends for ESP32 bb_epaper, ESP32 FastEPD, Nordic bb_epaper, and BG22
   bb_epaper.
3. Add `od_xfer` owner, mode, reset, abort, and timeout state.
4. Add `od_xfer_app` for the clock, END-ACK completion barrier, displayed-etag persistence, and
   target diagnostics.
5. Call `od_xfer_service()` from every target pump.
6. Test the shared core against a fake panel that records every call and can fail each operation.

Exit gate: a fake-panel test can prove power-neutral validation, exact write order, ACK-before-
refresh ordering, and cleanup after every injected failure. No command has moved yet.

### Phase 5 — Promote direct write on all targets

1. Implement shared START/DATA/END hooks using `od_xfer` and the panel HAL.
2. Preserve ESP32's direct auto-END profile; preserve explicit END on Nordic/BG22.
3. Split START into validation and hardware activation so invalid size/capability never touches the
   panel.
4. Track received compressed bytes separately from written decompressed bytes.
5. Refuse overrun rather than truncate while still accepting the valid final prefix.
6. Gate refresh on successful END reply and the target completion barrier.
7. Commit/clear displayed etag only after the refresh result.
8. Cut over ESP32, Nordic, and BG22 one at a time; after each cutover, delete that target's direct
   protocol state and hook bodies.

Required state tests:

- START lengths 0..5, exact/off-by-one declared totals, initial compressed bytes, and unavailable
  compression;
- replacement START, wrong owner, DATA with no transfer, empty DATA, raw/compressed completion,
  overrun, sink failure, timeout, disconnect, and core reset;
- END lengths 0..6, both refresh modes, optional etag, incomplete raw and compressed streams;
- START/DATA/END ACK sealing or queue failure at every reply position;
- completion barrier success, timeout, API-unavailable recovery, and owner death;
- refresh success, refresh timeout, refresh command error, BUSY never asserted, and panel abort;
  and
- all color schemes and both ESP32 panel backends.

Exit gate: target command files contain no direct-wire parsing or reply construction, and the
monolithic display files retain only panel/backend operations.

### Phase 6 — Promote legacy partial write

1. Implement `od_xfer_partial.c` over the shared stream and panel HAL.
2. Parse the asserted big-endian header with checked span operations; do not overlay host-endian
   integers.
3. Centralize validation order so error precedence is deterministic.
4. Use the same DATA and END hooks as direct, branching only by the single transfer mode enum.
5. Make etag clearing/commit rules explicit and tested.
6. Compile the state completely out on BG22 while retaining its four-byte unsupported NACK.
7. Delete ESP32 and Nordic partial contexts, parsing, zlib pumps, and panel-independent validation.

Required tests cover each error precedence, arithmetic overflow, byte alignment, row padding,
old/new etags, two-plane boundary splits at every byte position, inline START data, raw/compressed
streams, all refresh selectors, incomplete END, wrong owner, replacement by direct/PIPE START,
disconnect, timeout, and reply substitution.

Exit gate: no target owns `PartialStreamContext`; only panel region setup and plane mapping remain
behind the HAL.

### Phase 7 — Promote PIPE

1. Implement the shared negotiation/parser and capability-off behavior.
2. Implement the reorder queue and SACK builder against the shared full/partial sink.
3. Derive queue storage from `OD_PIPE_MAX_W` and wire payload size.
4. Enforce owner, negotiated frame, window, and state on every DATA and END.
5. Preserve raw full auto-completion and explicit completion for compressed/partial transfers.
6. Preserve fatal NACK silence after failure and ordinary retransmission via SACK.
7. Keep LAN refusal in the shared hook before any transfer state changes.
8. Cut over ESP32 and Nordic, then delete both target PIPE state machines and ESP32 PIPE structs.
9. Keep BG22's START unavailable response and silent DATA/END with no reorder allocation.

Required deterministic tests:

- every START length, version, flag, capability, total, partial extension, and negotiation boundary;
- requested W/N values 0, 1, 16, 17, 32, 33, and 255;
- effective frames below minimum, exactly minimum, 243, 244, and 245;
- sequence wrap at 254/255/0/1;
- every in-window arrival permutation for small windows 1..4;
- gap open/close, duplicate queued frame, duplicate consumed frame, mask bit 0 and bit 31, and no
  phantom pre-transfer ACK bits;
- reorder collision and overflow guards;
- cadence ACK, immediate gap ACK, duplicate ACK rate limiting, and tail SACK;
- compressed/raw/partial sink failure mapping;
- reply substitution at START, cadence SACK, gap SACK, tail SACK, END, and refresh status;
- fatal error followed by DATA silence, new START recovery, disconnect, timeout, and wrong owner;
- raw full auto-END, compressed explicit END, partial explicit END, incomplete holes, and etag
  commit/clear;
- PIPE-over-LAN refusal without disturbing a BLE-owned transfer; and
- W=16 and W=32 compile/runtime profiles.

Add a model-based test that generates loss, duplication, reordering, and wrap while comparing the
firmware receiver's accepted byte stream and SACKs to a simple reference model. Keep the existing
Python sender tests and add end-to-end randomized sender/receiver traces across the shared C model.

Exit gate: there is one PIPE implementation, and capable targets contain only sizing macros,
transport ingress, and panel HAL code.

### Phase 8 — Delete seams that no longer earn their existence

1. Remove the seven image-transfer declarations from `od_cmd_app.h` if the shared modules now
   define them directly for every target.
2. Remove target wrappers that only forward a span unchanged.
3. Remove obsolete target structs, globals, zlib buffers, duplicate helpers, and raw response
   literals.
4. Remove stale include paths and per-target source-list entries.
5. Add source-tree checks preventing reintroduction of target protocol state names.
6. Recount source and static memory against the Phase 0 baseline.

Exit gate: searching target trees for PIPE SACK construction, `PartialStreamContext`, NFC chunk
buffers, direct byte counters, or zlib poll loops finds no protocol implementation.

## 8. Commit and review structure

Use small commits with one observable purpose:

1. protocol symbols/prose and vectors;
2. trace fixtures and baselines;
3. geometry shared source/tests;
4. geometry target cutovers/deletion;
5. inflate backend/pump tests;
6. inflate target cutovers/deletion;
7. NFC shared state/tests;
8. NFC target HALs/cutovers/deletion;
9. panel HAL implementations and fake-panel tests;
10. common transfer owner/reset/timeout;
11. direct shared handler;
12. one direct target cutover per commit;
13. partial shared handler;
14. one partial target cutover per commit;
15. PIPE shared handler/model tests;
16. one PIPE target cutover per commit;
17. capability-off and dead-seam cleanup; and
18. documentation, measurements, and hardware evidence.

A target cutover commit must build with the old source removed. A commit that merely redirects a
hook while retaining callable old state is not a completed cutover.

## 9. Build and static-analysis gates

Every phase runs:

- the full host CTest suite;
- protocol header synchronization and generated Python-constant checks;
- all ESP32 board builds, including classic W=16 and all four silicon families;
- all Nordic boards: nRF52840, nRF54L15, and nRF54LM20A;
- the BG22 headless and production-source host profiles plus the real ARM link;
- compile permutations for PIPE, partial, NFC, zlib, RXQ, and direct-auto-END profiles;
- C and C++ linkage checks with exceptions/RTTI disabled where production disables them;
- ASan/UBSan host runs for geometry, direct, partial, PIPE, NFC, and inflate tests;
- fuzzing of START and DATA parsers plus stateful command sequences; and
- a map-file/static-RAM comparison against the Phase 0 baseline.

No new heap use is allowed. No variable-length stack object is allowed. No target may gain a
second full image buffer. BG22 must retain at least its baseline heap-inclusive headroom; a phase
that consumes that 480-byte margin does not pass because later stack use is not represented by the
static size report.

## 10. Hardware verification matrix

All rows record board ID, silicon, bootloader, build SHA, panel/controller, storage layout,
transport, encryption state, negotiated MTU/frame, raw host transcript, device log, sniffer trace
where required, elapsed time, and PASS/FAIL.

### 10.1 Silicon and backend rows

| Row | Required hardware distinction | Transfer coverage |
|---|---|---|
| ESP32 classic N4 | reduced W=16, constrained DRAM, no WiFi build | raw/compressed direct and PIPE, loss/reorder |
| ESP32-C3 | RISC-V, no PSRAM profile | raw/compressed direct and PIPE |
| ESP32-C6 | different BLE/WiFi silicon | raw/compressed direct and PIPE |
| ESP32-S3 bb_epaper | reference BLE plus WiFi/LAN | all image modes, LAN direct and PIPE refusal |
| ESP32-S3 FastEPD | runtime alternate panel backend | full direct/PIPE and supported partial behavior |
| nRF52840 | Adafruit/UF2 boot path | direct, partial, PIPE W=32, encryption |
| nRF54L15 | MCUboot/NCS path | direct, partial, PIPE W=32, NFC if wired |
| nRF54LM20A | distinct silicon/board and SoC NFCT | direct, partial, PIPE, NFCT NFC |
| EFR32BG22C222F352GM40 | 32 KB RAM, no kernel | direct, compression, TNB132M NFC, unsupported partial/PIPE |

One Nordic board cannot substitute for all three Nordic silicon/bootloader rows, and one ESP32-S3
cannot substitute for classic, C3, and C6 builds. If hardware is unavailable, the row remains open;
a build is not recorded as a hardware pass.

### 10.2 Required scenarios

For every capable row:

- plaintext and encrypted direct upload;
- compressed and uncompressed transfer;
- width requiring row padding;
- END ACK observed before refresh begins;
- successful and forced-timeout refresh;
- disconnect during START, middle DATA, END barrier, and refresh;
- replacement START and reconnect with no stale state;
- transfer timeout recovery without reboot;
- API failure follows platform recovery rather than locking the application; and
- subsequent authentication/config read/upload succeeds.

For partial-capable rows:

- etag match and mismatch;
- aligned and invalid rectangles;
- old/new plane boundary splits;
- compressed partial;
- full, fast, and partial refresh selectors; and
- failure clears etag and forces the next client operation to a full upload.

For PIPE-capable rows:

- negotiated W/N/frame values;
- forced loss, duplication, reordering, and retransmission;
- sequence wrap;
- tail shorter than ACK cadence;
- fatal DATA NACK followed by silence;
- raw full auto-completion;
- compressed and partial explicit END; and
- encrypted retransmission with a fresh CCM nonce.

For NFC-capable rows:

- read at 218 bytes and refusal/truncation behavior at 219;
- inline and 512-byte chunked writes;
- disconnect/reconnect during chunk assembly;
- invalid record type and hardware failure; and
- correct record observed by an independent NFC reader.

For ESP32 LAN:

- plaintext and TLS direct upload with a 4,092-byte data chunk;
- no app-layer CCM inside TLS;
- PIPE START refusal and DATA/END silence;
- a refused LAN PIPE frame does not disturb a BLE-owned transfer; and
- a LAN disconnect only aborts a LAN-owned transfer.

## 11. Quantitative acceptance criteria

Record after each phase:

- shared transfer LOC added;
- target transfer LOC removed;
- net handwritten production LOC;
- count of target-defined transfer command hooks;
- count of target zlib pump loops;
- count of target wire-response literals;
- `.text`, `.rodata`, `.data`, `.bss`, heap-inclusive BG22 result, and stack high-water;
- transfer throughput and retransmission count; and
- hardware matrix rows passed.

Final minimums:

- one direct, one partial, one PIPE, one NFC command implementation;
- one inflate pump;
- zero target protocol state structs;
- zero disabled PIPE/partial/NFC buffers on BG22/ESP32 as applicable;
- no static-RAM regression on BG22;
- no throughput regression above 5% without a documented reliability or RAM benefit; and
- net production-source deletion. Test growth is reported separately and is not counted as a
  failure of de-duplication.

## 12. Stop conditions

Stop a phase and do not begin the next when:

- canonical and client interpretations of a byte differ;
- a target requires a behavior macro not tied to shipped compatibility or a real capability;
- a fake HAL starts encoding wire policy;
- target panel code must inspect an opcode, error code, SACK, etag frame layout, or compression
  header;
- shared code includes an SDK/vendor header;
- BG22 static memory grows beyond baseline headroom;
- an END ACK cannot be shown to precede refresh;
- disconnect/reset leaves any transfer or NFC assembler active;
- a reply failure is followed by hardware mutation or a success frame;
- a capability-off build links its disabled buffers; or
- old and new production state machines remain callable at the same phase exit.

## 13. Definition of done

Transfer promotion is complete only when all of the following are true:

1. Shared code builds as plain C against host fakes and all three SDK targets.
2. Geometry, compression, direct, partial, PIPE, and NFC have dedicated production-source tests.
3. The wire corpus and Python client suites agree with firmware replies and state transitions.
4. Every capability-off permutation has explicit wire tests and zero large disabled allocations.
5. Every silicon/backend/transport hardware row is complete.
6. ACK-before-refresh ordering is supported by a packet trace, not inferred from logs.
7. Target code contains hardware adaptation only.
8. Old target transfer state, hooks, pump loops, geometry math, and NFC assemblers are deleted.
9. Source and memory measurements show net deletion without a BG22 regression.
10. A clean build from each supported toolchain produces release artifacts with no stale target
    source entry or duplicate backend.
