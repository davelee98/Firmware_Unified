# Transfer-plane promotion plan

**Original date:** 2026-08-17

**Reconciled:** 2026-08-18

**Source snapshot:** `main` at `d2190cc`

**Status:** Phase 1 software-complete on `feat/transfer-promotion`. The shared inflate pump and
target-selected backend seam are implemented and all five target-local pump loops are gone. Target
builds pass, but no compressed transfer has run on hardware, so the Phase 1 exit gate remains open
and Phases 2-5 are deliberately not stacked on top of it.

This is the active plan for the remaining transfer-plane work. It supersedes the transfer sequence
in `PLAN_MIGRATION_ENDGAME_2026-08-17.md` and the geometry/compression phases in the original
version of this file. The detailed C14 and color plans remain the record of what already landed.

### Implementation checkpoint — 2026-08-18

- Added `od_zlib_pump` and `od_inflate_app`; ESP32 keeps ROM tinfl selection while Nordic and BG22
  bind the portable C14 engine.
- The pump owns push/poll/final progression and sink refusal. Scratch remains target-owned: 2,048
  bytes on the measured ESP32 profile and 256 bytes on Nordic/BG22. Link-map inspection shows one
  scratch buffer and one inflater history window per image.
- The host pump suite covers every input split, output capacities 1..256, cross-boundary
  back-references, exact/short/long output, truncation, checksum failure after output, final twice,
  reset-before-first-push, reset after failure, empty input, sink refusal and a deliberately wrong
  backend output count.
- Software evidence: the complete gcc (39/39), clang including fuzz (42/42), and ASan/UBSan
  (39/39) host suites pass; all 11 ESP32 configurations and their sdkconfig baseline pass; all
  three Nordic boards build; and BG22 builds at 251,236 bytes flash and 32,284 bytes static RAM
  (480 bytes headroom). Leak detection was disabled for the clang/fuzz and sanitizer runs because
  LeakSanitizer cannot operate under this environment's ptrace wrapper.
- Hardware evidence remains absent: no board or serial device was attached. Per the phase ordering,
  direct/partial promotion cannot begin until compressed direct, partial and PIPE exercise this pump
  on the capable hardware rows.

## 1. Reconciliation result

The original plan had seven material conflicts with current `main`:

1. **Geometry is already promoted.** `shared/core/od_color.{c,h}` is the sole direct-stream
   geometry authority. There will be no `od_xfer_geometry` module and no second geometry pass.
2. **The portable inflater is already promoted, but its pump is not.**
   `shared/core/od_zlib_inflate.{c,h}` and its host suite landed in C14. Five target-local
   push/poll/sink loops and ESP32 backend selection still need promotion.
3. **Direct and legacy partial cannot be cut over independently.** `0x76` starts partial state,
   but `0x71` and `0x72` continue and finish it. A shared direct handler cannot replace those
   opcodes while partial state remains target-local without a temporary dual-owner router. The
   production cutover is therefore one **legacy-stream unit**: direct plus partial on ESP32 and
   Nordic, direct plus the existing unsupported-partial response on BG22.
4. **A second panel vtable is not justified.** The repository's one deliberate vtable remains the
   ESP32-only `od_panel_ops`. Shared transfer policy will call a plain-C link-time target seam in
   `od_xfer_app.h`; no `shared/hal/od_hal_panel.h` and no runtime registry are added.
5. **The proposed `end_ack_on_air()` contract overstated what software can prove and contradicted
   `od_txq`.** ESP32 and Nordic deliberately proceed after the 250 ms flush deadline with the ACK
   retained for late delivery; BG22 uses its stricter two-second flush/TX-idle recovery path. The
   shared seam preserves those profiles. Only a packet trace can establish “on air before
   refresh.”
6. **A universal 15-minute timeout would be a behavior change.** ESP32 has that watchdog;
   Nordic and BG22 do not. The promotion records a start time and exposes reset/abort state, but
   preserves target timeout policy. Unifying deadlines is separate reliability work.
7. **Canonical protocol edits are outside this repository's authority.** The sibling repositories
   are read-only references. Missing names and stale prose are external follow-ups, not transfer
   implementation commits. This plan pins existing bytes locally and does not assign new wire
   meanings.

The resulting order is:

1. retire current hardware debt and freeze production behavior;
2. finish the compression promotion by sharing the pump and engine-selection seam;
3. promote the legacy direct/partial stream together;
4. promote PIPE;
5. promote NFC; and
6. remove obsolete seams and close the hardware/release matrix.

## 2. Objective and boundaries

Completion means:

- one shared zlib push/poll/finalize pump;
- one shared owner/arbitration record for image transfers;
- one shared direct-write state machine for `0x70`/`0x71`/`0x72`;
- one shared legacy-partial state machine for `0x76` plus its `0x71`/`0x72` continuation,
  compiled out on BG22;
- one shared PIPE state machine for `0x80`/`0x81`/`0x82`, compiled out on BG22;
- one shared NFC endpoint state machine for `0x83`, compiled out on ESP32;
- no target-owned transfer wire parsing, SACK construction, transfer ownership, byte accounting,
  etag policy, compression-finalization policy, or NFC chunk assembly;
- target command hooks reduced to one-line calls into shared policy or an explicit unsupported
  response;
- disabled capabilities contributing no reorder queue, partial context, NFC staging buffer, or
  decompression scratch storage; and
- each unit independently built, hardware-qualified and revertible before the next starts.

The following remain target-owned:

- panel/controller selection, power, bus setup, address-window quirks and plane mapping;
- FastEPD buffering and refresh behavior;
- BUSY polarity, watchdog feeding, refresh waiting, touch suspension and power keep-alive;
- transport drain/recovery primitives;
- NFC controller I/O and record storage; and
- platform reset, disconnect and link-drop policy.

This work does **not**:

- modify any sibling repository;
- edit the vendored protocol headers by hand;
- add Gray8 wire behavior—`OD_COLOR_SCHEME_GRAY8 == 9` remains an unsupported placeholder;
- enable dual-CS on BG22;
- alter BG22's fixed 256-byte boot row-buffer or silent-overflow policy;
- add a new FastEPD failure detector or failure path;
- redesign refresh as asynchronous work;
- change session cryptography or nonce rules; or
- make PIPE legal on LAN.

## 3. Current ground truth

### 3.1 Landed shared prerequisites

Current `main` already supplies:

- `od_dispatch`, `od_reply`, `od_txq`, `od_rxq`, `od_session` and the immutable reply identity
  `{origin, tag}`;
- C13's BG22 shared command/config/session path;
- `od_zlib_inflate` as the canonical bounded portable zlib engine;
- `od_color` as checked direct-stream geometry; and
- shared boot rendering, which is separate from transfer format policy.

`od_color_direct_geometry()` is consumed by all three transfer implementations. It returns
`OD_COLOR_UNSUPPORTED` for Gray8, RGB schemes and unknown values. `BWGBRY_SPLIT` geometry does not
authorize a target that rejects dual-CS hardware. ESP32's exact FastEPD ED103TC2 adapter remains
the one approved target-owned geometry override; it does not authorize Gray8.

### 3.2 Remaining production ownership

| Area | ESP32-IDF | Nordic/Zephyr | EFR32BG22 |
|---|---|---|---|
| Direct/full transfer | `src/display_service.cpp` | `src/opendisplay_display.cpp`, `src/od_cmd_direct.c` | `opendisplay_display.cpp`, `od_cmd_silabs.c` |
| Legacy partial | `src/display_service.cpp` | `src/opendisplay_display.cpp`, `src/od_cmd_direct.c` | explicit unsupported hook |
| PIPE | `src/display_service.cpp` | `src/opendisplay_pipe_write.cpp` | explicit unsupported hooks |
| NFC command state | explicit unknown hook | `src/od_cmd_nfc.c` | `od_cmd_silabs.c` |
| Inflate pump loops | two in `display_service.cpp` | two in `opendisplay_display.cpp` | one in `opendisplay_display.cpp` |
| Portable inflate engine | shared C14 source | shared C14 source | shared C14 source |
| Direct geometry | shared `od_color` | shared `od_color` | shared `od_color` |

Current scope indicators, not deletion promises:

| File | Lines at reconciliation |
|---|---:|
| `targets/esp32-idf/src/display_service.cpp` | 3,351 |
| `targets/nordic-zephyr/src/opendisplay_display.cpp` | 1,363 |
| `targets/nordic-zephyr/src/opendisplay_pipe_write.cpp` | 595 |
| `targets/nordic-zephyr/src/od_cmd_nfc.c` | 206 |
| `targets/efr32bg22-slc/opendisplay_display.cpp` | 953 |
| `targets/efr32bg22-slc/od_cmd_silabs.c` | 443 |
| `shared/core/od_zlib_inflate.c` | 694 |
| `shared/core/od_color.c` | 115 |

### 3.3 Capability matrix

| Capability | ESP32-IDF | Nordic/Zephyr | EFR32BG22 |
|---|---:|---:|---:|
| Direct write | yes | yes | yes |
| Streaming zlib inflate | yes | yes | yes |
| Legacy partial `0x76` | yes | yes | no |
| PIPE full/partial | yes | yes | no |
| NFC endpoint | no | yes | yes |
| BLE | yes | yes | yes |
| LAN direct write | yes | no | no |
| LAN PIPE | deliberately refused | n/a | n/a |

ESP32 normally uses a 32-frame PIPE window and uses 16 on classic ESP32 N4. Nordic uses 32. BG22
has 32 KB total RAM and currently only about 480 bytes of heap-inclusive static headroom, so
capability-off state must disappear from its linked image rather than merely remain unused.

### 3.4 Existing test evidence

- `tests/host/color_test.c` owns shared geometry, including odd widths, split halves and explicit
  Gray8 rejection.
- `tests/host/zlib_inflate_test.c` owns the portable C14 engine contract.
- `tests/host/pipe_write_test.c` compiles Nordic's production PIPE implementation but is not a
  shared PIPE oracle and does not cover the complete state space.
- The dispatch corpus owns routing and reply bytes, not transfer continuation state.
- There is no shared production-source direct, partial, NFC or pump test yet.

Before transcribing behavior, diff the live sibling authority against the unified snapshot. For
ESP32 algorithms `../Firmware/` is authoritative; Nordic and host differences are evidence that
must be explained, not silently copied. `../py-opendisplay` determines what the client actually
sends and accepts. These repositories remain read-only.

## 4. Wire and lifecycle behavior to preserve

### 4.1 Common rules

- Handlers receive a plaintext body after the shared session gate.
- Application ACKs use `od_reply()`; hard NACKs use `od_reply_plain()`.
- Every multi-frame state is owned by the full immutable `{origin, tag}`. Wrong-owner DATA/END is
  inert and cannot mutate, refresh or reset another owner's transfer.
- Replacement START aborts the previous image-transfer mode before arming the new one.
- Reply failure is part of the handler result. If an ACK is substituted or cannot be queued, no
  later success or panel refresh may follow and the newly armed state is unwound.
- `od_core_reset()` clears portable transfer/NFC state before the target tears down hardware.
- No target callback may retain `od_cmd_ctx_t` or its reservation after dispatch returns.

### 4.2 Direct write: `0x70`, `0x71`, `0x72`

- START body lengths 0..3 select uncompressed mode; the nonzero bytes are tolerated and ignored.
- A body of at least four bytes selects zlib and begins with the four-byte little-endian exact
  decompressed size. Remaining START bytes are initial compressed input.
- Every current build links a usable inflater. Following the ESP32 authority, the START bytes—not
  the advertised `transmission_modes` bits—select compression. Nordic/BG22's current extra config
  gate is removed as an explicitly tested divergence resolution; conforming hosts are unaffected
  because they already use the advertised modes to choose what to send.
- DATA streams raw or compressed bytes. No target writes beyond `od_color`'s expected total.
- ESP32's legacy uncompressed direct path auto-completes when the expected total is reached;
  Nordic and BG22 wait for explicit END. Preserve this through `OD_XFER_DIRECT_AUTO_END` rather
  than separate state machines.
- END consumes byte 0 as the refresh selector when present. On partial-capable targets a new etag
  is recognized only when at least five bytes are present and is big-endian; trailing extension
  bytes remain tolerated. BG22 ignores the optional etag tail and allocates no displayed-etag
  state because it cannot consume that state through a partial transfer.
- Successful ordering is END ACK, target barrier, physical refresh, then `00 73` or `00 74`.
- Compressed completion is exact: DONE early, output overrun, truncated input or checksum failure
  refuses END.
- Gray8 and all other `od_color`-unsupported schemes fail START without panel activation.

Invalid raw overrun behavior is not normalized during source movement. The shared implementation
must first characterize the authority and each target: ESP32 currently accepts the valid prefix
and reaches auto-END, while explicit-END profiles can ignore trailing data after their expected
total. A stricter NACK is a separate wire-policy change.

### 4.3 Legacy partial: `0x76` followed by `0x71`/`0x72`

- The asserted 17-byte START header is parsed explicitly as big-endian fields; it is not overlaid
  on a native struct.
- Only compressed flag bit 0 is accepted.
- `old_etag` is nonzero and matches the displayed etag; `new_etag` is nonzero.
- The panel/config must admit partial operation. Today `od_color` marks only MONO eligible; the
  promotion does not broaden that policy.
- Width/height are nonzero, the rectangle fits with checked arithmetic, and X/width are multiples
  of eight.
- Expected data is two row-padded 1-bpp planes. A partial transfer never auto-completes.
- Geometry, stream, incompleteness, END-reply or refresh failure clears the displayed etag.
  Successful refresh commits `new_etag` only afterwards.
- END accepts at most its optional refresh byte; the etag was fixed at START.
- BG22 preserves its four-byte unsupported NACK and allocates no partial state.

### 4.4 PIPE: `0x80`, `0x81`, `0x82`

- START parses the asserted ten-byte little-endian request and optional twelve-byte partial
  extension. Existing tolerated trailing bytes remain tolerated.
- The response reports device maxima. Both peers apply the minimum rule to window, cadence and
  frame.
- Window and cadence floor at one, cadence does not exceed window, and the window does not exceed
  the 32-bit SACK reach.
- Sequence arithmetic wraps modulo 256. In-order data reaches the shared full/partial sink;
  ahead-within-window data waits; duplicates are discarded and SACKed; outside-window input is
  fatal.
- A fatal DATA NACK leaves subsequent DATA silent until START/reset. A replay/out-of-window frame
  rejected by `od_session` also remains silent under `OD-S1`.
- END emits a tail SACK, validates completeness, emits `00 82`, crosses the target barrier,
  refreshes and reports `00 73`/`00 74`.
- Only uncompressed full-frame PIPE may auto-complete. Compressed and partial PIPE require END.
- ESP32 continues to refuse PIPE on LAN without disturbing a BLE-owned transfer.
- BG22 preserves `FF 80 04 00` on START and silence/UNKNOWN for DATA and END. Value `0x04` remains
  target-specific; this repository does not assign it a canonical meaning.

The shared implementation closes the existing negotiated-frame enforcement gap: the complete
plaintext PIPE DATA frame must fit `frame_eff`, and the reorder payload width is derived as
`PIPE_MAX_FRAME - PIPE_FRAME_OVERHEAD` (241), not the current 248. This affects malformed or
nonconforming clients and receives an isolated test/commit inside the PIPE unit.

The hardware checklist calls small/sub-cadence tails a live stall, while current Nordic code
immediately SACKs an already-consumed duplicate and the current Python sender explicitly performs
that duplicate probe. Phase 0 must reproduce the claim on current HEAD or retire it as stale. Do
not add a timer or preserve a supposed defect without a failing trace.

### 4.5 NFC: `0x83`

- READ is `[00][83][80][type][len:2 BE][data]` and exposes at most 218 tag bytes in both plaintext
  and encrypted sessions.
- Single WRITE and chunked START/DATA/END retain their deployed status and error bytes.
- The chunk buffer is 512 bytes and is owned by `{origin, tag}`.
- Replacement START, owner death, overflow, hardware failure, reply failure and core reset clear
  the assembler.
- A short END remains retryable if the production differential fixtures show that behavior; do
  not silently reset it merely to simplify cleanup.
- ESP32 returns `OD_CMD_UNKNOWN`, emits nothing and allocates neither response nor staging buffer.

### 4.6 Compression

- C14's public `od_zlib_stream_*` engine API and `od_zlib_inflate.{c,h}` filenames remain stable.
- The host emits a zlib wrapper with a nine-bit window. The portable engine rejects wider CMF
  values; the ESP32 tinfl backend's current wider acceptance remains a documented divergence for
  separate resolution, not a pump-promotion side effect.
- The shared pump owns push/poll/finalize progression, exact output accounting and sink refusal.
- Backend choice remains a build profile. Exactly one selected backend is called, although the
  unselected portable PURE object may be link-discarded on tinfl builds.
- Scratch storage is caller/profile supplied so the pump promotion does not create a second
  output buffer. BG22 and Nordic retain 256 bytes; an ESP32 2,048-byte profile must be measured.

## 5. Resulting architecture

### 5.1 Shared modules

Already landed and reused:

```text
shared/core/od_color.[ch]
shared/core/od_zlib_inflate.[ch]
```

New production modules:

```text
shared/core/od_zlib_pump.[ch]       backend-neutral push/poll/finalize loop
shared/core/od_inflate_app.h        target-selected engine seam
shared/core/od_xfer.[ch]            owner, arbitration, reset and common stream state
shared/core/od_xfer_direct.[ch]     0x70/0x71/0x72 policy
shared/core/od_xfer_partial.[ch]    0x76 and partial continuation policy
shared/core/od_xfer_app.h           target panel/lifecycle/barrier seam
shared/core/od_pipe.[ch]            0x80/0x81/0x82 negotiation, reorder and SACK
shared/core/od_nfc.[ch]             0x83 parsing and 512-byte chunk assembly
shared/core/od_nfc_app.h            target NFC read/write seam
```

There is no `shared/compress/`, `od_xfer_geometry`, generic panel vtable or runtime registration.
Every source is listed exactly once in `shared/sources.cmake`. Sources needing target functions use
an APP seam tier, following `APP_SESSION` and `APP_RXQ`; PURE remains C-library/protocol only.

Shared modules export policy entry points such as `od_xfer_direct_start()`. Each target continues
to define every `od_cmd_app_*` hook so C11's link-time completeness rule remains intact, but a
promoted hook is a one-line call and contains no wire bytes or state transition.

### 5.2 Common image-transfer state

One portable object represents mutually exclusive image modes:

```c
typedef enum {
    OD_XFER_IDLE,
    OD_XFER_DIRECT_FULL,
    OD_XFER_DIRECT_PARTIAL,
    OD_XFER_PIPE_FULL,
    OD_XFER_PIPE_PARTIAL,
    OD_XFER_FATAL
} od_xfer_mode_t;

typedef struct {
    od_xfer_mode_t mode;
    od_reply_t owner;
    uint32_t started_ms;
    uint32_t expected_bytes;
    uint32_t received_bytes;
    uint32_t written_bytes;
    bool compressed;
#if OD_CAP_PARTIAL
    struct od_xfer_partial_state partial;
#endif
} od_xfer_t;
```

There are no independent direct-active, partial-active and PIPE-hardware-active booleans. PIPE owns
only sequencing/reorder metadata and feeds the same full/partial sink. It does not own a second
decompressor or byte counter.

The state machine is single-consumer, like dispatch/session/txq. It adds no lock. The complete
reply identity is copied at START; the reservation itself is never retained.

### 5.3 Target transfer seam

`od_xfer_app.h` is a set of link-time C functions, not a vtable. Its implementation owns hardware
resources and target recovery. The final names may tighten during header-first tests, but the seam
must express these operations and no wire policy:

```c
bool od_xfer_app_panel_info(struct od_xfer_panel_info *out);
bool od_xfer_app_begin_full(const od_color_geometry_t *geometry);
#if OD_CAP_PARTIAL
bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               uint32_t plane_bytes);
#endif
bool od_xfer_app_write(const uint8_t *data, uint32_t len);
void od_xfer_app_abort(void);
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner);
bool od_xfer_app_refresh(uint8_t mode, bool *completed);
#if OD_CAP_PARTIAL
uint32_t od_xfer_app_displayed_etag(void);
void od_xfer_app_set_displayed_etag(uint32_t etag);
#endif
uint32_t od_xfer_app_now_ms(void);
```

`od_xfer_panel_info` includes the checked geometry descriptor. Ordinary adapters obtain it from
`od_color_direct_geometry()`. ESP32 may apply only the already-tested ED103TC2 FastEPD exception;
BG22 may reject split geometry under its no-dual-CS policy. The target maps parts/planes to
controller operations and owns plane-switch side effects. Shared code owns byte accounting and
verifies that every sink call consumes exactly the offered bytes.

The barrier has two outcomes: proceed or abort. It preserves current profiles:

- ESP32/Nordic: enqueue success is mandatory; drain for up to 250 ms, retain late ACKs and proceed
  on the existing timeout result, including the existing short dwell.
- BG22: retain its two-second queue/TX-idle requirement and transport recovery on failure.

The function is deliberately not named `on_air`. Software queue/controller acceptance is not an
RF observation; the hardware gate uses a trace to prove ordering.

### 5.4 Inflate and NFC seams

`od_inflate_app.h` forwards reset/push/poll/error/count to the selected target backend. Nordic,
BG22 and portable ESP32 profiles forward to C14; tinfl ESP32 profiles forward to the existing
adapter. The shared pump accepts caller-owned scratch and a checked sink callback.

`od_nfc_app.h` contains only read/write operations:

```c
bool od_nfc_app_read(uint8_t *type, uint8_t *data, uint16_t *len_io, uint16_t cap);
bool od_nfc_app_write(uint8_t type, const uint8_t *data, uint16_t len);
```

NDEF/controller details remain target code. Shared NFC owns record validation, bounds, replies,
owner binding and reset.

### 5.5 Compile-time surface

Retain and validate:

```text
OD_CAP_PARTIAL
OD_CAP_PIPE
OD_CAP_RXQ
OD_CAP_DUAL_CS
```

Add only facts the new code actually consumes:

```text
OD_CAP_NFC
OD_PIPE_MAX_W
OD_PIPE_MAX_N
OD_XFER_DIRECT_AUTO_END
```

Do not add `OD_CAP_ZLIB`: all current targets link an engine, and START flags/shape select whether
the transfer is compressed. Do not add `OD_INFLATE_OUTPUT_CHUNK`: scratch size is an argument, not
shared object layout.

Compile-time assertions cover W/N/SACK bounds, reorder slots, RX/TX capacity, 241-byte reorder
payloads, wire struct sizes and capability booleans. Capability-off builds prove the large objects
are absent by map/symbol inspection.

### 5.6 Plain-C decision

`od_xfer`, `od_xfer_partial`, `od_pipe` and `od_nfc` remain plain C.

This is the required re-evaluation from architectural decision 1. The shared objects own no C++
resource: panel/NFC/transport resources live behind target functions, portable state has one
explicit reset path, and every fallible operation can use single-exit `goto cleanup`. A destructor
would not close anything the shared code is allowed to own. If implementation forces a vendor
handle or nested acquired resource into shared state, stop—the seam is wrong rather than C being
insufficient.

## 6. Execution sequence and gates

### Phase 0 — current-HEAD evidence and hardware debt

Before moving another production call site:

1. Diff the relevant `../Firmware` and `../Firmware_NRF54` transfer files against current target
   snapshots; record deliberate unified adaptations separately from donor drift.
2. Add fixed and malformed continuation fixtures for every behavior in section 4 that is not
   already executable through the dispatch corpus.
3. Re-run `tools/check.sh --targets`; any skip is not a pass.
4. Capture current `.text`, `.rodata`, `.data`, `.bss`, BG22 heap-inclusive size, transfer
   throughput and refresh timing.
5. Finish the `xiao_nrf52840` current-stack rows that will otherwise lose their pre-promotion
   reference: plaintext PIPE, `CMD_PARTIAL_WRITE`, current-HEAD config round-trip,
   disconnect/interruption recovery and `OD-S1` replay injection.
6. Flash BG22 and run C13 Gate 2, including raw and compressed direct write, END ordering, NFC
   218/219 boundaries and capability-off partial/PIPE responses.
7. Run C14 compressed uploads on Nordic, BG22, one portable-engine ESP32 and one tinfl ESP32.
8. Exercise the relevant `od_color` rows on available real panels. Gray8 has no hardware row.
9. Reproduce or retire the checklist's small-tail PIPE stall on current HEAD with a raw transcript.

Exit gate: every behavior that will be deleted has an executable fixture or a consciously recorded
hardware gap. If a required board is unavailable, implementation may remain planned but is not
called hardware-qualified and the one-subsystem hardware gate is not waived.

### Phase 1 — finish compression promotion

1. Add `od_inflate_app.h` and target forwarding adapters without renaming C14's public API.
2. Add `od_zlib_pump` with caller-owned scratch, exact output accounting, finalization and sink
   refusal.
3. Characterize the existing five loops before replacing them, including empty pushes, output
   split boundaries, DONE/NEEDS_INPUT progression and errors after partial output.
4. Replace one target at a time; delete its loops in the same commit.
5. Keep tinfl selection and the wider-CMF divergence unchanged.
6. Prove no second history window or output buffer is retained in each link map.
7. After each capable target cutover, run raw/compressed direct; on ESP32/Nordic also run compressed
   partial and PIPE. Follow the repository target order: ESP32, Nordic nRF54 class, BG22, then the
   nRF52840 qualification row.

Required host tests cover every input split, every output capacity from 1 through the target chunk
size, back-references across boundaries, exact/short/long output, bad wrapper/checksum/Huffman data,
truncation, final twice, reset after failure and sink refusal.

Exit gate: one pump exists, all five target loops are gone, backend choice is explicit and every
target family has a recorded compressed hardware result.

### Phase 2 — promote the legacy direct/partial stream

1. Write header-first fake-target tests for the common state, target seam and reply ordering.
2. Implement `od_xfer`, `od_xfer_direct` and `od_xfer_partial` together. Partial storage and code
   are under `#if OD_CAP_PARTIAL`; the unsupported reply remains available without that state.
3. Consume `od_color_geometry_t`; do not recreate format switches or byte math.
4. Use `od_zlib_pump` for both full and partial compressed streams.
5. Split START validation from target activation. Invalid geometry, etag, flags or size touches no
   panel resource. Compression admission follows section 4.2's authority decision.
6. Preserve target direct-auto-END and barrier profiles as data/configuration.
7. Make every target `od_cmd_app_direct_*` and `od_cmd_app_partial_start` a thin shared call and
   delete its old parsing/state/reply code in the same target cutover.
8. Update `od_core_reset()` and target disconnect cleanup so portable state resets once and target
   hardware aborts once.
9. Cut over and hardware-test each target before the next. On partial-capable targets, direct and
   partial switch together because they share DATA/END.

Required tests include START lengths 0..5; compressed inline input; supported and rejected color
schemes; replacement START; wrong owner; zero/stray DATA; exact, incomplete and overlong streams;
all END lengths and refresh selectors; reply substitution at each position; barrier proceed/abort;
refresh success/timeout; reset/disconnect; partial flag/etag/rectangle precedence; arithmetic
overflow; plane-boundary splits at every byte; raw/compressed partial; and BG22 capability-off RAM.

Hardware must cover plaintext/encrypted raw and compressed direct, ACK-before-refresh trace,
disconnect/reconnect, replacement START and a subsequent successful command. ESP32/Nordic also run
partial etag match/mismatch, aligned/invalid rectangles, both plane boundaries and failure-clears-
etag. ESP32 additionally runs plaintext LAN and TLS-LAN direct writes with a 4,092-byte DATA chunk
and verifies that a LAN disconnect affects only a LAN-owned transfer. BG22 runs the unsupported
response and confirms no partial or displayed-etag state in the map.

Exit gate: target code owns hardware operations only; no target parses `0x70`, `0x71`, `0x72` or
`0x76`, owns transfer byte counters, finalizes zlib, or constructs their replies.

### Phase 3 — promote PIPE

1. Implement shared START negotiation, owner checks, sequence arithmetic, reorder queue and SACK.
2. Feed the Phase 2 full/partial sink; do not create a PIPE decompressor or duplicate byte totals.
3. Derive reorder payload from `PIPE_MAX_FRAME - PIPE_FRAME_OVERHEAD` and enforce negotiated frame
   size on every DATA frame.
4. Preserve fatal-NACK silence, raw-full auto-END, compressed/partial explicit END and ESP32 LAN
   refusal.
5. Resolve the current-tail evidence from Phase 0 with a test, not an assumed timer.
6. Cut over ESP32 and Nordic independently and delete both target PIPE machines. BG22 keeps only
   its explicit unsupported wrapper and links no reorder state.

Deterministic tests cover every START length/version/flag/capability/total; W/N values 0, 1, 16,
17, 32, 33 and 255; frame bounds; sequence wrap; all small-window arrival permutations; gap close,
duplicates, mask bits 0/31, cadence/tail SACKs; reply substitution; fatal silence; replacement,
disconnect and wrong owner; raw/compressed/partial completion; W=16/W=32 profiles; LAN refusal
inertness; and capability-off builds.

Add a simple reference receiver and model-based traces with loss, duplication, reorder and wrap.
Keep Python sender tests as the independent peer. Hardware runs forced loss/reorder/retransmission,
tail below cadence, sequence wrap and `OD-S1` replay on ESP32 and Nordic.

Exit gate: one PIPE machine exists; target code contains sizing facts, ingress and hardware seam
only; BG22 pays zero PIPE state.

### Phase 4 — promote NFC independently

1. Add `OD_CAP_NFC` with Nordic/BG22 enabled and ESP32 disabled.
2. Add `od_nfc_app.h` adapters over existing controller operations.
3. Implement parsing, response framing, record validation, 218-byte read cap, 512-byte assembly,
   owner binding and reset in `od_nfc`.
4. Reduce each `od_cmd_app_nfc` to a shared call or ESP32's explicit unknown behavior.
5. Delete Nordic and BG22 command buffers/parsers in the same cutover.

Tests cover every subcommand and error; lengths 0, 1, 120, 121, 218, 219, 512 and 513; all record
types; trailing single-write extensions; zero-length DATA; wrong owner; recycled tag; replacement
START; short END retry; overflow; read/write failure; reply failure; reset; and encrypted/plain
read-size equality.

Hardware verifies 218/219 read behavior, inline and 512-byte chunked write, disconnect during
assembly, hardware failure and independent-reader contents on Nordic/BG22. ESP32 map/symbol tests
prove no NFC buffers.

Exit gate: only controller adaptation remains target-owned and BG22 static RAM is at or below its
Phase 0 baseline after old buffers are removed.

### Phase 5 — cleanup and release evidence

1. Delete obsolete transfer declarations, structs, pump buffers, target constants and build
   entries after their last caller disappears.
2. Update `shared/sources.cmake` arrival comments and structural ratchets to reject a second
   transfer parser, pump, reorder state or NFC assembler.
3. Re-run the complete software gate and inspect every link map.
4. Record source and memory deltas by unit, not only for the final squash.
5. Complete the per-silicon/backend/transport hardware matrix or leave each unavailable row
   explicitly open; never convert a build into a hardware pass.

## 7. Commit structure

Use independently reviewable commits; do not land unused scaffolding:

1. Phase 0 fixtures and measurements;
2. inflate selection seam plus pump tests;
3. one pump target cutover/deletion per commit;
4. transfer headers, fake target and direct/partial state tests;
5. one legacy-stream target cutover/deletion per commit;
6. PIPE shared state/model tests;
7. one PIPE target cutover/deletion per commit;
8. NFC shared state/tests;
9. one NFC target cutover/deletion per commit; and
10. ratchets, measurements and final evidence.

A cutover commit builds with the replaced source removed or reduced to hardware-only operations.
No commit leaves two callable implementations for one mode. Tests may retain frozen reference code
as a fixture, clearly separated from production linkage.

## 8. Software and hardware gates

Every phase runs `tools/check.sh --targets` and reads the final summary. Required coverage includes:

- gcc, clang, ASan and UBSan host suites;
- all 11 ESP32 configurations and sdkconfig baseline;
- all three Nordic board builds;
- BG22 production-source tests and real ARM link;
- dispatch corpus profiles and Python wire corpus;
- capability permutations and map/symbol checks;
- parser/state-machine fuzzing where added; and
- mutation checks that prove the new tests detect wrong owner, wrong byte total, lost reset,
  dropped reply status, bad SACK and capability-off allocation.

No new heap use, variable-length stack object, full image buffer or unexplained BG22 static-RAM
increase is allowed.

Current hardware evidence is incomplete and must not be flattened into “Nordic passed”:

| Row | Current position | Transfer promotion obligation |
|---|---|---|
| ESP32-S3 bb_epaper | shared stack exercised raw/encrypted; C14/color-specific matrix incomplete | all promoted modes plus LAN/TLS and trace |
| ESP32-S3 FastEPD | color adapter software-tested; transfer failure detection remains follow-up | full direct/PIPE and supported partial behavior |
| portable-engine ESP32 | C14 compressed case not run | compressed direct after pump cutover |
| `xiao_nrf52840` | encrypted PIPE only on current C8-C11 stack; matrix incomplete | plaintext/encrypted direct, partial, PIPE loss/replay |
| `xiao_nrf54l15` / `xiao_nrf54lm20a` | build-only, never flashed | Nordic silicon/boot/NFC distinctions |
| EFR32BG22 | C13 build-only, never flashed | direct/compression/NFC and unsupported partial/PIPE |

Final release qualification also retains classic ESP32 W=16, C3, C6, S3 bb_epaper/FastEPD,
nRF52840, both nRF54 boards and BG22 as distinct rows. Hardware availability may leave a row open;
it may not weaken a shared invariant or be reported as a pass.

## 9. Quantitative acceptance

Record after each unit:

- shared production LOC added and target production LOC removed;
- target-owned transfer state structs and wire-response literals remaining;
- pump-loop count;
- `.text`, `.rodata`, `.data`, `.bss` and BG22 heap-inclusive size;
- stack high-water where available;
- throughput, retransmissions and refresh time; and
- hardware rows passed/open.

Final minimums:

- one zlib pump, direct machine, partial machine, PIPE machine and NFC machine;
- zero target protocol state structs or target zlib pump loops;
- zero disabled PIPE/partial/NFC large objects;
- no BG22 static-RAM regression against the recaptured Phase 0 baseline;
- no unexplained throughput regression above 5%; and
- net handwritten production-source deletion. Test growth is reported separately.

## 10. Stop conditions

Stop the current unit when:

- authority, client and target interpret a byte differently and the divergence is not recorded;
- direct cutover would leave target-local partial continuation behind;
- a target seam starts parsing opcodes, errors, SACKs, etags or compression headers, or grows a
  color-format switch beyond the existing exact FastEPD ED103TC2 adapter;
- shared code includes a vendor/framework header;
- a second panel vtable or runtime registry appears;
- a reply failure is followed by panel mutation or later success;
- the barrier changes a target's current timeout/recovery profile without a separate decision;
- reset/disconnect leaves portable or hardware transfer state active;
- capability-off code retains its large buffer;
- BG22 exceeds its recaptured static-RAM baseline without an approved tradeoff;
- Gray8 acquires geometry, packing or target admission;
- BG22 split/dual-CS support or a new FastEPD failure path enters through this work;
- a sibling repository is modified; or
- old and new production implementations remain callable at a phase exit.

## 11. Definition of done

The transfer promotion is complete only when:

1. Shared code builds as plain C against host fakes and all target toolchains.
2. Pump, direct, partial, PIPE and NFC each have production-source state tests.
3. Shared replies and transitions agree with frozen firmware fixtures and the Python client.
4. Every capability-off permutation has explicit wire tests and zero large disabled allocation.
5. Every available capable target has passed the unit's hardware gate; unavailable rows remain
   explicit release debt.
6. ACK-before-refresh ordering is supported by packet evidence where hardware permits capture.
7. Target transfer code contains hardware adaptation only.
8. Old target state, parsing, pump loops, SACK code and NFC assemblers are deleted.
9. Source/memory measurements show net deletion without a BG22 regression.
10. A clean `tools/check.sh --targets` run reports no failure and no skip.
