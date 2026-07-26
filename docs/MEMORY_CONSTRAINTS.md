# Memory, portability, and dependency policy

What in `shared/core` risks not fitting the EFR32BG22, with the numbers, and the dependency
rules for the two Arduino panel libraries (bb_epaper, FastEPD) under ESP-IDF. Companion to
SHARED_API_DESIGN.md and DIVERGENCE_MATRIX.md; everything here is measured from the checked-in
builds, not estimated.

## The BG22 budget (measured)

From `arm-none-eabi-size` on the shipped `cmake_gcc/build/base/opendisplay-bg22.out` and the
`.map`:

```
text 236316   data 492   bss 31792     (bss as reported includes the NOLOAD heap)
```

- **RAM: 32 KB total, ~97 % committed at link.** Real static footprint ≈ 21.2 KB
  (`.stack` 2752 + `.bss` + `.data` + `.noinit`), heap = **10 576 B** (`heap_size = 0x2950`),
  and `heap_size` is literally *whatever RAM is left* (`linkerfile.ld`: `RAM_end − __HeapBase`).
  **Every static byte `shared/core` adds comes straight out of the 10.3 KB heap**, which the BT
  stack already draws 3150 B from (`SL_BT_CONFIG_BUFFER_SIZE`).
- **Flash: 236.8 KB of 272 KB, ~87 %, ~35 KB headroom.** Enough for shared code, but not
  unlimited — measure `.text` after each subsystem swap.
- App code owns **~11.6 KB of the 21.2 KB static** (61 %). That is the negotiating room.

### The app-owned statics that `shared/core` competes with (BG22, measured via `nm`)

| Bytes | Symbol | What |
|---:|---|---|
| 2064 | `s_cfg_rec` | NVM3 config record (16 hdr + 2048 data) |
| 1676 | `od_zlib_stream` state `s` | inflate: 512 B window + 608 B literal tree + 320 B lengths |
| 844 | `s_od_global_config` | parsed `struct GlobalConfig` |
| 640 | `s_session` | `EncryptionSession` — **512 B of it is `replay_window[64]`** |
| 640 | `s_buttons[32]` | button state |
| 520 | `s_nfc_write_chunk` | incl. `data[512]` |
| 513+512 | `s_crypto_payload_buf` + `s_plain_buf` | CCM staging |
| 256 | `s_decompression_chunk` | inflate output staging |

Realistic recoverable RAM if `shared/core` is careful: **~6 KB** — unify the config staging
buffer with the CCM plaintext buffer, and shrink the replay window (below).

## What in `shared/core` risks not fitting — concretely

1. **The PIPE reorder queue — the single biggest risk.** Firmware's `pipeReorder[33]` at
   4 + 248 B/slot is **~8.3 KB `.bss`** (4.3 KB in the `PIPE_SMALL_DRAM_WINDOW` 17-slot form).
   That is a quarter of the BG22's entire RAM for a subsystem the chip does not implement and
   the spec says it must not. **`OD_PIPE_ENABLE=0` must `#if` the array out of existence**, not
   merely skip the handlers — a linked-but-unused 8.3 KB array is an instant link failure here.

2. **The zlib window — a wire contract, not a knob.** BG22 pins
   `OPENDISPLAY_ZLIB_WINDOW_BITS=9` → **512 B**, statically (`USE_HEAP_WINDOW=0`). A 32 KB
   window (bits 15) is a third of the chip's RAM and can never exist. The inflater *rejects* any
   stream whose CMF header declares a window larger than its compile limit
   (`od_zlib_stream.c:641-644`), so this is not a local sizing choice — **the host encoder
   (`py-opendisplay`) must cap `windowBits ≤ 9` for any stream a 9-bit device will receive.**
   Correction to the workspace-level note "existing targets pin 32 KB windows for legacy-client
   compatibility": that is false for all but one board. The measured reality —

   | Target / board | `WINDOW_BITS` | Window | Alloc |
   |---|---:|---:|---|
   | EFR32BG22 | 9 | 512 B | static |
   | nRF54L15 | 9 | 512 B | static |
   | ESP32 (default, most envs) | 9 | 512 B | heap |
   | ESP32-S3 `env:esp32-s3-E1004` **only** | 15 | 32 KB | heap |

   So `shared/compress` treats the window as a per-target macro with **512 B as the documented
   floor**. The host side already complies unconditionally — `py-opendisplay` encodes
   `window_bits = 9` for every device and rejects its own output if the header advertises more
   (DESIGN_REVIEW_2026-07-25.md F5) — so the wire contract holds today and the E1004's 15-bit
   build is host-unreachable dead capability. What is left open is narrower; see below.

3. **`MAX_CONFIG_SIZE` — DECIDED 2026-07-25: 4096 fleet-wide, BG22 included.** It is a single
   product-wide value, not a per-target macro: the host may send up to 4096 bytes of config to
   any device. This ends the silent-truncation class and makes `py-opendisplay` correct as it
   stands (it already hardcodes 4096 — DESIGN_REVIEW F5), but **the cost lands entirely on the
   BG22 and it is the largest single RAM ask in this document.** Before the Silabs swap
   (migration step 3), measure it:

   | Item | Today | At 4096 | Delta |
   |---|---:|---:|---:|
   | `s_cfg_rec` NVM3 record (16 hdr + data) | 2064 B | 4112 B | **+2048 B** |
   | `opendisplay_config_buf()` read scratch (`MAX_CONFIG_SIZE`-shaped) | 2048 B | 4096 B | **+2048 B** |

   Against a **10 576 B heap** and a realistic recoverable budget of ~6 KB (above), +4 KB is
   more than the whole negotiating margin. Two mitigations are already identified and now become
   load-bearing rather than optional: unify the read scratch with the CCM plaintext buffer (2.5
   says one shared scratch, which removes the second row), and shrink the 512 B replay window to
   a 64-bit bitmap (§ below, ~8 B). That converts the ask from ~4 KB to ~2 KB, which the ~6 KB
   budget absorbs.

   **Also verify on the NVM3 side, not just the RAM side:** a 4112-byte object must fit the
   configured NVM3 max-object-size and the NVM3 instance's flash capacity. Neither is a given —
   check both before committing the Silabs build, because this is the one target where the
   decision can fail to *fit* rather than merely cost.

   If it does not fit after the two mitigations, that is a finding to escalate — the decision is
   fleet-wide 4096, and reverting BG22 to 2048 re-opens the divergence deliberately rather than
   by discovery. Historical context follows.

   BG22 stored 2048 (NVM3 record 2064 B) while nRF/ESP32 stored 4096, so a config that fit nRF
   but not BG22 was silently truncated on BG22 (DIVERGENCE_MATRIX 2.7). The decision above
   removes that split rather than making it discoverable: with one fleet-wide value there is no
   per-device limit for a host to interrogate, which is why `MAX_CONFIG_SIZE` also drops out of
   the capability-reporting bytes in ARCHITECTURE.md § "The gap, and a proposed fix".

4. **Any assumption of heap.** The core must run on caller/static buffers only. No `malloc`
   exists in BG22 app code today; the one `malloc` in the inflater is compiled out
   (`USE_HEAP_WINDOW=0`). A `shared/core` that mallocs anything excludes this target.

5. **Any assumption of a scheduler or blocking.** No kernel, no `k_msleep`, no work queue.
   Handlers run inline in the BGAPI callback. This is why SHARED_API_DESIGN.md makes refresh a
   non-blocking `refresh_start` + `refresh_busy` pump instead of a blocking
   `wait_refresh(timeout)` — the Silabs 60 s in-callback block already drops the BLE link
   mid-refresh, and a shared core must not standardize that.

6. **The replay window is 8× oversized.** `replay_window[64]` of `uint64_t` = 512 B, linearly
   scanned, for a ±32 accept window. A 64-bit bitmap tracking the ±32/64 window is ~8-16 bytes.
   On a 32 KB chip this is free RAM worth reclaiming — but the change is security-sensitive
   (the exact-counter `diff==0` replay hole must be closed at the same time), so make it
   deliberately in `od_session.c`, not as an incidental optimization.

### RAM sizing rules for `shared/core`

- Every buffer sized by a target macro with a documented floor; nothing sized for the biggest
  target. `MAX_CONFIG_SIZE`, `OPENDISPLAY_ZLIB_WINDOW_BITS`, `OD_RX_QUEUE_DEPTH`,
  `OD_PIPE_*` are the knobs.
- These are **plain preprocessor constants** — do not build the config surface around Kconfig
  (BG22 has none; it sets them via `target_compile_definitions`). Kconfig on ESP-IDF/Zephyr is
  merely *how those two set* the constants.
- Measure `.bss`/`.text` on BG22 after every subsystem swap (MIGRATION.md verification bar), not
  at the end — a few KB over the hand-written original is a link failure, not a tuning task.

## Dependency policy: bb_epaper and FastEPD under ESP-IDF

Both are Arduino-origin panel libraries; both must work under ESP-IDF (the ESP32 target drops
Arduino). Findings verified against the clones at `/home/davelee/opendisplay/{bb_epaper,FastEPD}`.

### Both are already ESP-IDF-capable — this is not a port

- **bb_epaper** selects its backend by `#ifdef` in `bb_epaper.cpp:40-50`; the non-Arduino
  default is `esp_idf/esp_generic.inl` (`driver/gpio.h` + `driver/spi_master.h` + `esp_timer.h`,
  with Arduino-compat shims). Root `CMakeLists.txt` is already
  `idf_component_register(... REQUIRES driver esp_timer)`. Backend `.inl` inventory in the clone:
  `arduino_io`, `esphome_io`, `mem_io`, `rpi_io`, `esp_idf/{esp_generic,esp_main_io,s3_ulp_io,
  c6_ulp_io}`, `ch32v`, `rpi_pico`. **`nrf54_zephyr_io.inl` and `silabs_efr32_io.inl` do NOT
  exist upstream** — they live only in the NRF54/Silabs vendored copies. So the single vendored
  copy under `third_party/bb_epaper/` must be **assembled** from upstream (for `esp_idf/`) plus
  the two downstream backends. Worth upstreaming so it stays a checkout, not a fork.
- **FastEPD** is *primarily* an ESP-IDF library; `arduino_io.inl` is a misnomer for its ESP32
  backend, which has a full `#ifndef ARDUINO` branch (`driver/gpio.h`, `esp_timer.h`,
  `driver/i2c_master.h`). `<Arduino.h>`/`<SPI.h>`/`<Wire.h>` are all `#ifdef ARDUINO`-guarded.
  The panel bus is pure ESP-IDF (`esp_lcd_panel_io`, `esp_lcd_new_i80_bus`, `parlio_tx` for
  C5). Root `CMakeLists.txt`: `idf_component_register(... REQUIRES driver esp_timer esp_lcd)`.
  Working **C** IDF examples exist (`examples/esp_idf/papers3_demo/main/papers3_demo.c`).

**Neither ships an `idf_component.yml`**, so neither is on the ESP Component Registry — the IDF
build must vendor them or add them via `EXTRA_COMPONENT_DIRS`/submodule. Given the "assemble one
bb_epaper copy" requirement above, **vendor both under `third_party/`**, consistent with the
scaffold.

### The duplicate-symbol collision is real, follows to ESP-IDF, and has no opt-out

Both libraries ship `Group5.cpp` and `bb_ep_gfx.inl`, registered *unconditionally* in both
`CMakeLists.txt` files. The genuinely colliding symbols (identical C++ mangled name):

- **Group5, 5 symbols**: `G5DECODER::init`, `G5DECODER::decodeLine`, `G5ENCODER::init`,
  `G5ENCODER::encodeLine`, `G5ENCODER::size` (`bb_epaper/src/Group5.cpp` vs
  `FastEPD/src/Group5.cpp` — bodies identical, only license header differs).
- **bb_ep_gfx, 4 symbols**: `bbepUnicodeString`, `bbepStretchAndSmooth`, `RotateCharBox`,
  `bbepUnicodeTo1252` (identical signatures; the rest are spared only by C++ overloading on the
  differing state-struct pointer type `BBEPDISP*` vs `FASTEPDSTATE*`).

Neither library offers a `NO_GROUP5`/`BB_EP_NO_GFX` opt-out. The shared include guards are
useless (separate translation units). So the PR-#120 workaround `-Wl,--allow-multiple-definition`
reproduces under ESP-IDF's CMake exactly as under PlatformIO — the link picks the first
definition and drops the rest.

**Policy recommendation.** `-Wl,--allow-multiple-definition` is a blunt instrument: it silences
*all* duplicate-symbol errors, including future accidental ones, and which copy wins is
link-order-dependent. Since both `Group5` copies are byte-identical in behaviour and only one
library's drawing API is actually used per board (upstream comment: "we only use FastEPD
buffer+update APIs"), prefer in priority order:

1. **Compile Group5.cpp / bb_ep_gfx.inl from exactly one library.** In the IDF component for the
   *unused*-gfx library, drop those two files from its `SRCS` list (a one-line `CMakeLists.txt`
   edit in the vendored copy — allowed, it is our fork under `third_party/`). This removes the
   collision at the source instead of papering over it, and keeps the linker strict for real
   duplicate bugs. Verify the two `Group5.cpp` are behaviourally identical first (they are:
   only the license header differs) so dropping one is safe.
2. If (1) is impractical because both drawing APIs are needed on the same board, keep
   `--allow-multiple-definition` but **scope it** and add a build-time check that no *new*
   duplicate symbols appear beyond the known Group5/bb_ep_gfx set.

Also flagged: `g5enc.inl` declares `g5_encode_*` `static` in bb_epaper but `extern` in FastEPD —
three more collisions appear if bb_epaper ever drops `static`. Pin versions (below) so that
can't surprise a build.

### Version pinning — fix the asymmetry

Current `Firmware` `upstream/main`:

- **FastEPD** = `bitbank2/FastEPD@^2.2.0` — a floating caret range (clone HEAD is `2.2.0-9`,
  9 commits past the tag).
- **bb_epaper** = a bare git URL with **no ref** on the default `[env]` (clone HEAD is
  `2.1.11-4`, 4 commits past its tag), except `env:esp32-s3-E1004` which pins a specific
  `limengdu` fork commit.

**Pin both to exact commits in this repo.** They are vendored under `third_party/`, so pin by
the committed tree, and record the upstream commit SHA + tag in a provenance header
(`bb_epaper 71f6e70 / v2.1.11+4`, `FastEPD 770e168 / v2.2.0+9`). A caret range and a bare git URL
are both non-reproducible, and the Group5 `static`/`extern` asymmetry above means an unpinned
bump can introduce link collisions silently.

### FastEPD is ESP32-only and PSRAM-mandatory — scope it accordingly

FastEPD's parallel path uses the ESP32 i80/parlio LCD peripheral (`library.properties:
architectures=esp32`) and **hard-`#error`s without PSRAM** (`FastEPD.inl:29-31`), allocating its
framebuffers from `MALLOC_CAP_SPIRAM` (~259 KB + ~130 KB for 960×540). It cannot be a universal
panel layer — it is one backend for the two IT8951 panels (`OD_PANEL_IC_ED103TC2_*`, ids
3000/3001) on PSRAM-equipped S3 boards. Its flat C API (`bbepInitPanel(FASTEPDSTATE*, …)`,
exercised from `app_main` in the IDF examples) means it sits behind `od_hal_panel` with **no C++
shim** — but only as the `needs_framebuffer=true` ops table, selected by `panel_ic_type`, never
as the default. bb_epaper remains the streaming (`needs_framebuffer=false`) default.

## Open questions needing a human decision

- **The zlib window — the contract itself is no longer open** (corrected 2026-07-25). The host
  encodes `window_bits = 9` unconditionally and rejects its own output if the header says
  otherwise (`py-opendisplay` `encoding/compression.py:14`, `device.py:389, 1842` —
  DESIGN_REVIEW_2026-07-25.md F5), so everything on the wire is 9 bits and the E1004's 15-bit
  build is host-unreachable dead capability. What still needs a human is narrower: does anyone
  ever want >9 bits, and where does the per-device "max windowBits" value live — which is the
  capability-discovery problem (ARCHITECTURE.md § "Capabilities are discovered by
  interrogation"), not a compression one.
- ~~**`MAX_CONFIG_SIZE` 2048 vs 4096.**~~ **DECIDED 2026-07-25 — 4096, product-wide, a global
  cap rather than a per-target macro.** The host may send up to 4096 bytes to any device,
  BG22 included. See item 3 above for the BG22 cost (~+4 KB naive, ~+2 KB after the two
  required mitigations) and the NVM3 checks that must precede the Silabs swap.
- **`CMD_NFC_ENDPOINT` placement.** In the header, implemented only on NRF54/Silabs, absent on
  Firmware. Core-behind-`OD_NFC_ENABLE`, or target-local? (SHARED_API_DESIGN.md assumes the
  former.)
- **Group5 de-duplication vs `--allow-multiple-definition`.** Recommendation is to compile the
  duplicated files from one library only; confirm both drawing APIs are not simultaneously
  required on any single board first.
- **Enforce the outer config CRC16?** It is advisory-only (logged, never rejected) on all three
  targets today. Starting to enforce it in `shared/core` is a behaviour change that could reject
  configs the host currently gets away with sending.
