# Transfer-plane promotion plan

**Original date:** 2026-08-17

**Reconciled:** 2026-08-18

**Source snapshot:** `main` at `d2190cc`

**Phase 2 revision:** `main` at `c41f3f5`

**Status:** Phase 1 landed on `main` at `a9a4ac5` and its hardware gate was marked cleared on
2026-08-18 in `docs/HARDWARE_VERIFICATION_CHECKLIST.md`. Phase 2's dormant shared machine landed
through PR #47 at `d83a41a`. The ESP32 step-10a software candidate now routes the four legacy
opcodes through shared policy and supplies the real hardware seam; it is not hardware-qualified.
Nordic may not begin until the ESP32 cutover gate passes. ESP32 retains only the target machinery
still called by PIPE, with PIPE and `od_xfer` made mutually exclusive owners of the singleton pump.

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
- Hardware evidence remains absent: no board or serial device was attached. Direct/partial
  production cutover remains blocked until the Phase 1 compressed direct, partial and PIPE results
  required by the hardware checklist are recorded.

### Phase 2 shared-side checkpoint — 2026-08-18

- On `feat/transfer-phase2`, steps 1-8 are implemented as dormant shared production code:
  `od_xfer`, `od_xfer_direct`, `od_xfer_partial` and the `od_xfer_app` seam. Dispatch still routes
  all four opcodes to the existing target hooks, and production reset/disconnect paths are
  unchanged; step 10a has not begun.
- The private singleton owns mode, immutable reply owner, timing and byte totals. DATA/END route
  once on that mode. The write seam receives a non-empty span and its pre-write offset; short
  consumption refuses the stream. Ownership is the complete `{origin, tag}` identity, and a
  read-only owner accessor exists for disconnect cleanup. This deliberately tightens ESP32's
  origin-only continuation rule: a reconnected instance cannot resume an abandoned transfer.
  The capability-off build retains only BG22's explicit partial unsupported reply.
- START calls a target pre-validation hook after replacing shared legacy-transfer state and before
  panel/geometry validation. That hook is the home for cancelling target-owned PIPE state and
  resetting transfer diagnostics even when START is rejected; it performs no panel activation.
  Abort calls carry a semantic reason so targets can preserve warm release for replacement and
  incomplete END while forcing power off for terminal stream/reset failures.
- END behavior pins application-vs-plain replies, enqueue failure, target drain, recovery exactly
  once, refresh and final status. A live-session host case proves the START ACK is sealed while its
  hard NACK remains plaintext.
- Host coverage includes raw truncation, START lengths 0..5, replacement, wrong-origin and
  stale-tag owners, empty DATA, compressed inline input and truncation, all direct and partial END
  lengths/selectors, refresh success/timeout/call failure, reply substitution at START/DATA/END/
  final status, partial validation/etag/plane offsets, raw and compressed partial, every
  plane-boundary split, controller-plane geometry and incomplete-END warm abort, every short sink
  consumption, both barrier failure positions, reset, ESP32 auto-END and the BG22 capability-off
  ABI. A live-session and
  security-disabled case cover START/DATA/END/final-status reply protection rather than START
  alone. The shared sources compile under both gcc and clang and under ASan/UBSan.
- ESP32's compressed-DATA failure previously emitted a hard NACK but returned `OD_CMD_OK`, unlike
  its partial path. Shared policy deliberately returns `OD_CMD_NACK`; only accepted frames may
  stamp session activity. This is a truthful-verdict normalization, not byte-level wire drift.
- All three target toolchains now compile the dormant transfer sources. ESP32 retains archive
  discard; Nordic and BG22 use compile-only object targets with their real capability definitions
  and deliberately provide no fake seam. The BG22 linked image remains 251,236 bytes flash and
  32,284 bytes static RAM (480 bytes headroom), proving the dormant objects add no image or RAM
  cost while the capability-off ABI still compiles under ARM GCC.
- Nordic's gate now makes `PURGE=always` literal for the L15 sysbuild composition by removing only
  its validated generated build directory before configuring. West's ordinary pristine pass can
  retain outer MCUboot/Partition Manager state across an application CMakeLists change and leave
  a PM placeholder unexpanded; incremental gate runs therefore remain equivalent to clean trees.
- `tools/check.sh --targets` passes 18/0/0 with the complete shared-side candidate: gcc, clang,
  ASan/UBSan, fuzz, the pinned Python wire corpus, all ESP32 configurations and sdkconfig
  baseline, all three Nordic boards, and the BG22 headless build. No hardware result is claimed.
- The production cutover is narrower than deletion of every target transfer helper. ESP32 and
  Nordic PIPE still call legacy direct/partial sinks, counters and finalization helpers. Phase 2
  removes their command policy for `0x70`, `0x71`, `0x72` and `0x76`, but retains and inventories
  only the target machinery required by PIPE. Phase 3 step 6 deletes that remainder together with
  the target PIPE machines. During this interval each PIPE START must displace a live `od_xfer`
  before touching `od_zlib_pump`, just as `od_xfer_app_prepare_start()` displaces a live PIPE in
  the reverse direction.

### Phase 1 hardware-gate clearance — 2026-08-18

- The project marked every Phase 1 pump row cleared in the authoritative hardware checklist.
- This clearance unblocks Phase 2 step 10a. It does not pre-approve any Phase 2 target cutover or
  replace that cutover's plaintext/encrypted, recovery, ownership and refresh-ordering matrix.

### Phase 2 ESP32 step-10a candidate — 2026-08-18

- The four `od_cmd_app` hooks are one-line bridges to `od_xfer_direct_start()`, `od_xfer_data()`,
  `od_xfer_end()` and `od_xfer_partial_start()`. ESP32's target-local legacy START/DATA/END parsing,
  ownership and reply construction are deleted.
- `display_service.cpp` supplies the complete hardware seam. Full and partial writes derive plane
  selection from the shared pre-write offset; target state retains geometry, rectangle and current
  controller plane but no legacy protocol byte counter. The shared owner also supplies the target
  watchdog's origin and its read-only start timestamp.
- Arbitration is bidirectional before pump use: PIPE START resets a live `od_xfer`, while
  `od_xfer_app_prepare_start()` releases target PIPE hardware and clears its state before a legacy
  START can activate. Disconnect/reset calls `od_xfer_reset()` before the common core teardown.
- Explicit Phase 3 debt remains because target PIPE calls it directly: `PipeWriteState`,
  `PipeReorderSlot`, `PartialStreamContext`, the `directWrite*` PIPE hardware fields, target PIPE
  zlib finalization, and the full/partial PIPE sink, geometry, activation and refresh helpers.
  None is reachable as command policy for `0x70`, `0x71`, `0x72` or `0x76`.
- Software evidence: the gcc host suite passes 43/43, and the clang and ASan/UBSan suites pass
  with leak detection disabled under the ptrace-based runner. All 11 ESP32 configurations build
  and pass the sdkconfig baseline; all three dormant Nordic configurations and the BG22 headless
  image also build. FastEPD and bb_epaper images link the shared machine and real seam; map
  inspection finds the shared entries in the image and no retired handler. BG22 remains at
  251,236 bytes flash and 32,284 bytes static RAM. Hardware evidence is open, so Nordic remains
  blocked by repository order.

## 1. Reconciliation result

The original plan had seven material conflicts with current `main`:

1. **Geometry is already promoted.** `shared/core/od_color.{c,h}` is the sole direct-stream
   geometry authority. There will be no `od_xfer_geometry` module and no second geometry pass.
2. **The portable inflater was already promoted, but its pump was not at reconciliation.**
   `shared/core/od_zlib_inflate.{c,h}` and its host suite landed in C14. Phase 1 subsequently
   promoted the five target-local push/poll/sink loops and ESP32 backend selection.
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
- promoted transfer opcodes routed directly to shared policy, with target completeness enforced by
  the link-time transfer seam rather than duplicate command wrappers;
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
| Inflate pump loops | shared `od_zlib_pump` | shared `od_zlib_pump` | shared `od_zlib_pump` |
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
- Scratch storage is caller/profile supplied, so the pump adds no second output buffer. BG22 and
  Nordic retain 256 bytes; the measured ESP32 tinfl profile retains 2,048 bytes.

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

Shared modules export command-signature policy entry points such as `od_xfer_direct_start()`.
At the Phase 2 exit, `OD_DISPATCH_OPCODE_ROWS` routes `0x70`, `0x71`, `0x72` and `0x76` directly to
those entry points and `od_cmd_app.h` no longer declares target hooks for them. The capability-off
`od_xfer_partial_start()` remains linked on BG22 and emits its existing explicit unsupported reply;
it allocates no partial state.

`od_xfer_data()` and `od_xfer_end()` intentionally live with the common state in `od_xfer.c`.
Those two opcodes continue either a full or partial stream, so they route once on the private
`od_xfer_mode_t` and call internal direct/partial operations. That mode switch is a state-machine
decision, not a second opcode map.

This narrows, rather than weakens, C11's link-time composition rule. Every remaining target command
hook is still mandatory. The four promoted opcodes instead require every target to define the
complete `od_xfer_app` seam, so an omitted hardware-policy decision is still a link error. During
the target-ordered cutover only, an existing target command hook may be a one-line bridge to the
shared entry point. After the last target passes its gate, the dispatch rows point directly to the
same functions and all four bridges are deleted. There is never a second opcode map.

### 5.2 Common image-transfer state

One private portable object represents mutually exclusive image modes. Its layout is not part of
`od_xfer.h` or any target seam:

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

The definition lives in a shared internal header used only by the transfer implementation modules.
All of those modules must be compiled with the same capability definitions. In particular,
`tests/host` compiles them into the existing separate `od_shared_silabs` library with
`OD_CAP_PARTIAL=0`; no object built with the default capability set may exchange transfer state
with that library. Host tests and link-map checks cover both layouts. This is the same ABI boundary
required by BG22's `OD_CONFIG_MAX_SIZE` build.

There are no independent direct-active, partial-active and PIPE-hardware-active booleans. PIPE owns
only sequencing/reorder metadata and feeds the same full/partial sink. It does not own a second
decompressor or byte counter.

The Phase 2-to-3 transition is the temporary exception, not the final design. ESP32 and Nordic
retain their target PIPE-active state and the target helpers PIPE directly calls until the shared
PIPE cutover. While that state exists, every target PIPE START first checks `od_xfer_active()` and
calls `od_xfer_reset()` if needed; only after shared legacy state and hardware are inactive may it
reset or push `od_zlib_pump`. In the other direction, every structurally valid legacy START reaches
`od_xfer_app_prepare_start()`, which cancels target PIPE before shared policy resets the pump. Thus
exactly one machine can own the singleton pump even during the staged migration.

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
uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data);
od_mut_span_t od_xfer_app_inflate_scratch(void);
void od_xfer_app_prepare_start(void);
void od_xfer_app_abort(od_xfer_abort_reason_t reason);
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner);
void od_xfer_app_barrier_abort(const od_reply_t *owner);
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
controller operations and owns plane-switch side effects. Shared code owns the sole stream byte
counter and passes its value as `stream_offset` before each write. The adapter selects and switches
planes from that offset and the geometry established by `begin_full()` or `begin_partial()`; it
must not retain a duplicate protocol counter. It returns the number of bytes consumed, and shared
code accepts the write only when that value equals `data.n`. Short consumption is a transfer
failure before any success reply.

`od_xfer_app_write()` has the precondition `data.n > 0`. Shared policy never calls the seam for an
empty raw DATA body or when an inflate push produces no output; it applies the frozen command
verdict and leaves accounting unchanged. Because every offered span is non-empty, a return of zero
is unambiguously a sink refusal rather than successful consumption of an empty write.

`od_xfer_app_inflate_scratch()` lends the target's one existing bounded output buffer to
`od_zlib_pump`; shared state neither owns nor duplicates it. ESP32 retains its selected 2,048-byte
profile and Nordic/BG22 retain 256 bytes.

`od_xfer_app_prepare_start()` runs for every structurally valid START, after an old shared legacy
transfer is replaced but before panel-info validation. It cancels target-owned PIPE state and
resets target diagnostics. This preserves ESP32's `resetPipeWriteState()` and
`imageWriteLogReset()` ordering, including rejected STARTs, without moving PIPE state or logging
into the legacy-transfer machine. The target PIPE START performs the opposite arbitration through
`od_xfer_active()` and `od_xfer_reset()`; this is target integration debt only until Phase 3 owns
both protocols.

`od_xfer_app_abort()` receives one of `REPLACED`, `START_FAILED`, `STREAM_FAILED`, `INCOMPLETE`,
`REPLY_FAILED`, `REFRESH_FAILED` or `RESET`. These are policy facts, not power commands: each
adapter maps them to its existing cleanup profile. ESP32 keeps replacement, START rejection and
bitplane incompleteness warm; mid-stream/final-inflate failure, reply failure and reset are
terminal force-off paths. Nordic and BG22 retain their current teardown behavior. No adapter may
collapse the reason before making its target-specific power/recovery decision.

The END sequence is explicit:

1. shared policy validates completion and queues the END acknowledgement through `od_reply()`;
2. if enqueue fails, shared policy calls `od_xfer_app_barrier_abort()` exactly once and neither
   refreshes nor emits another reply;
3. after a successful enqueue, `od_xfer_app_before_refresh()` drains according to the target
   profile and returns proceed or abort; and
4. on abort, shared policy calls `od_xfer_app_barrier_abort()` exactly once and neither refreshes
   nor emits another reply.

The target abort hook owns the existing recovery operations; shared policy owns the wire decision
and portable-state reset. The barrier preserves current profiles:

- ESP32/Nordic: retain the existing void flush/short-dwell behavior and return proceed
  unconditionally once the ACK was queued. `od_xfer_app_barrier_abort()` is a no-op apart from any
  already-required panel abort; no new timeout refusal is introduced.
- BG22: retain the two-second queue/TX-idle deadline. Return abort on either failure, and let
  `od_xfer_app_barrier_abort()` perform the existing display abort, transport reset and owner close.
  That path emits no additional reply. The same recovery hook runs when ACK enqueue itself fails.

The two abort functions are intentionally distinct. `od_xfer_app_abort(reason)` releases panel/write
state for replacement START, stream failure, reset or disconnect. `od_xfer_app_barrier_abort()` is
called only after the END acknowledgement commit is attempted and may additionally reset transport
state or close the reply owner, as BG22 requires. ESP32 and Nordic may implement both through one
target-private helper where their recovery is identical; the shared contracts remain separate so
an ordinary stream abort cannot acquire BG22's barrier-only transport side effects.

Every application ACK, success and refresh-timeout reply uses `od_reply()` and therefore follows
the live session's seal-or-plain policy. Protocol validation errors and explicit hard NACKs use
`od_reply_plain()`. Tests must assert this choice under both a live encrypted session and plaintext;
checking payload bytes alone is insufficient.

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
are absent by map/symbol inspection. Dispatch tests also pin the Phase 2 reservation budgets
(`DIRECT_START=1`, `DIRECT_DATA=2`, `DIRECT_END=2`, `PARTIAL_START=1`) before and after direct
routing replaces the temporary bridges; a budget change is a separate wire-policy decision.

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

Entry boundary: the Phase 1 gate in `docs/HARDWARE_VERIFICATION_CHECKLIST.md` was marked satisfied
on 2026-08-18, so step 10a may begin. Do not fold a Phase 1 pump correction into this phase; if later
hardware evidence exposes one, fix and re-qualify Phase 1 first.

Build the shared side as separately reviewable, revertible units. Steps 1-4 do not reroute a
production opcode:

1. Land `od_xfer_app.h`, the private state boundary and header-first fake-target tests. Pin write
   offsets/consumption, reply ordering, barrier recovery and reset before implementing a machine.
2. Land `od_xfer` ownership, arbitration and reset only. It owns the sole mode, owner, elapsed-time
   record and byte totals; it neither parses a command nor touches hardware.
3. Land `od_xfer_direct` and its direct-call host tests. Consume `od_color_geometry_t` and
   `od_zlib_pump`; do not recreate format switches, byte math, an inflater loop or a sink counter.
4. Land `od_xfer_partial` and its direct-call host tests. Partial storage and implementation are
   under `#if OD_CAP_PARTIAL`, while the capability-off entry point remains and emits the existing
   unsupported reply without partial state. Build all transfer modules again in
   `od_shared_silabs` with `OD_CAP_PARTIAL=0`, and prove the partial object/state is absent.
5. Split START validation from target activation. Invalid geometry, etag, flags, size or arithmetic
   touches no panel resource. Compression admission follows section 4.2's authority decision.
6. Make shared accounting authoritative. Every sink call receives the pre-write stream offset;
   accept only `consumed == offered`, and delete each target counter used only by the four legacy
   opcodes when its adapter is cut over. A counter or helper still called by target PIPE may remain
   until Phase 3, but it must not mirror or influence `od_xfer` accounting. Plane selection,
   address-window reissue and controller side effects remain target operations derived from that
   offset and the geometry established at START.
7. Implement the END sequence from section 5.3. Pin ACK-enqueue failure separately from drain
   failure, invoke target recovery exactly once, emit no substitute reply on either barrier abort,
   and never mutate or refresh the panel after failure.
8. Preserve direct-auto-END and the three target barrier profiles explicitly. ESP32/Nordic return
   proceed after their existing flush/dwell; BG22 alone can abort on its two-second drain/TX-idle
   deadline and performs its existing recovery through `od_xfer_app_barrier_abort()`.
9. Prepare and host-test the `od_core_reset()` and disconnect integration so portable state resets
   once and target hardware aborts once, but do not change a production cleanup path yet. The
   disconnect bridge uses `od_xfer_owner()` rather than retaining a target-side owner copy. Compile
   the transfer tier under each target toolchain before cutover; Nordic and BG22 may use a
   compile-only object target while dormancy is required. Do **not** land fake or forwarding
   `od_xfer_app` implementations merely to satisfy the linker: a truthful adapter must own the
   final hardware-only surface and delete the target protocol counters it replaces, so it lands
   atomically with that target's step-10 cutover. Apply reset/disconnect integration only in that
   cutover as well.
10. Split the production cutover at the PIPE dependency:

    - **10a — target cutover:** proceed in repository target order. For each target, implement the
      real adapter, replace its four command implementations with temporary one-line bridges to
      shared policy, and apply its reset/disconnect integration. On ESP32 and Nordic, also rewire
      PIPE START to displace a live `od_xfer` through `od_xfer_active()`/`od_xfer_reset()` before
      PIPE resets or pushes the singleton pump. Delete legacy-opcode parsing, ownership and reply
      construction in the same commit, but retain and inventory any target sink, counter, geometry
      or finalization helper still called by PIPE. Those retained pieces are PIPE implementation
      debt, not a second callable implementation of the four legacy opcodes. Pass that target's
      hardware gate before changing the next. Direct and partial switch together on capable
      targets because they share DATA/END; BG22 switches direct plus the capability-off partial
      reply. BG22's simpler no-PIPE cutover does not change the repository order: ESP32 remains the
      reference, followed by Nordic nRF54, BG22 and the nRF52840 qualification row.
    - **10b — PIPE-dependent deletion:** defer deletion of the target machinery inventoried solely
      for PIPE to Phase 3 step 6. It is not part of the Phase 2 exit gate, and no new legacy command
      path may call it after step 10a.
11. After all targets call the shared policy, route the four `OD_DISPATCH_OPCODE_ROWS` entries
    directly to `od_xfer_direct_start`, `od_xfer_data`, `od_xfer_end` and
    `od_xfer_partial_start`. Delete the four declarations from `od_cmd_app.h` and every temporary
    target bridge. Add a symbol ratchet forbidding their reintroduction and pin the unchanged
    1/2/2/1 reservation budgets in dispatch tests.

Required tests include START lengths 0..5; compressed inline input; supported and rejected color
schemes; replacement START; wrong owner; zero/stray DATA; exact, incomplete and overlong streams;
all END lengths and refresh selectors; reply substitution at each position; barrier proceed/abort;
ACK enqueue failure; recovery exactly once and no reply after barrier failure; refresh
success/timeout; reset/disconnect; partial flag/etag/rectangle precedence; arithmetic overflow;
empty raw DATA and inflate pushes that produce no output never calling the write seam; monotonic
non-empty offsets and short-consumption results from zero through one less than the offered length;
plane-boundary splits at every byte; raw/compressed partial; every application reply's sealed/plain
choice under both a live session and plaintext; identical dispatch outcomes before/after direct
routing; PIPE START during a live `od_xfer`, proving one shared reset/abort occurs before PIPE can
reset the pump; legacy START during a live PIPE, proving `od_xfer_app_prepare_start()` cancels PIPE
before shared policy can reset the pump; displaced-owner DATA/END remaining inert in both
directions; and BG22 capability-off code, ABI and RAM.

Hardware must cover plaintext/encrypted raw and compressed direct, ACK-before-refresh trace,
disconnect/reconnect, replacement START and a subsequent successful command. ESP32/Nordic also run
partial etag match/mismatch, aligned/invalid rectangles, both plane boundaries and failure-clears-
etag. ESP32 additionally runs plaintext LAN and TLS-LAN direct writes with a 4,092-byte DATA chunk
and verifies that a LAN disconnect affects only a LAN-owned transfer. BG22 runs the unsupported
response and confirms no partial or displayed-etag state in the map.

Exit gate: for `0x70`, `0x71`, `0x72` and `0x76`, target code owns hardware operations only; no
target parses those commands, owns their protocol accounting, finalizes their streams, constructs
their replies, or defines a command hook for them. The dispatch map names shared transfer entry
points directly, every target defines the complete transfer seam, and no capability-mismatched
transfer state crosses a library boundary. ESP32 and Nordic may temporarily retain only the
explicitly inventoried state and helpers still called by target PIPE. Tests prove that target PIPE
and `od_xfer` cannot be active together and that either START displaces the other before any reset
or push of the singleton `od_zlib_pump`; removing that PIPE-owned remainder is Phase 3 debt.

### Phase 3 — promote PIPE

1. Implement shared START negotiation, owner checks, sequence arithmetic, reorder queue and SACK.
2. Feed the Phase 2 full/partial sink; do not create a PIPE decompressor or duplicate byte totals.
   This removes the temporary cross-machine arbitration and makes the shared transfer owner the
   only authority that may reset or push `od_zlib_pump`.
3. Derive reorder payload from `PIPE_MAX_FRAME - PIPE_FRAME_OVERHEAD` and enforce negotiated frame
   size on every DATA frame.
4. Preserve fatal-NACK silence, raw-full auto-END, compressed/partial explicit END and ESP32 LAN
   refusal.
5. Resolve the current-tail evidence from Phase 0 with a test, not an assumed timer.
6. Cut over ESP32 and Nordic independently and delete both target PIPE machines. In the same
   target commit, delete the sinks, counters, geometry/finalization helpers and arbitration bridge
   inventoried in Phase 2 step 10a solely because target PIPE still called them. BG22 keeps only
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
5. one legacy-stream target cutover/reduction per commit, with PIPE-only remainder inventoried;
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
| ESP32-S3 bb_epaper | Phase 1 pump gate cleared; broader color/transfer matrix incomplete | all promoted modes plus LAN/TLS and trace |
| ESP32-S3 FastEPD | color adapter software-tested; transfer failure detection remains follow-up | full direct/PIPE and supported partial behavior |
| portable-engine ESP32 | Phase 1 compressed pump gate cleared | qualify the promoted transfer machine |
| `xiao_nrf52840` | Phase 1 compressed pump gate cleared; broader matrix incomplete | plaintext/encrypted direct, partial, PIPE loss/replay |
| `xiao_nrf54l15` / `xiao_nrf54lm20a` | Phase 1 nRF54-class pump gate cleared; broader distinctions remain | Nordic silicon/boot/NFC distinctions |
| EFR32BG22 | Phase 1 compressed-direct pump gate cleared; broader C13 matrix remains | direct/NFC and unsupported partial/PIPE |

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
- old and new production implementations for the same opcode remain callable at a phase exit; or
- target PIPE and `od_xfer` can be active together, or either can reset/push the singleton pump
  without first displacing the other owner.

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
