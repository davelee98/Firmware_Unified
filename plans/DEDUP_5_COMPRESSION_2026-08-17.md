# De-duplication 5 — COMPRESSION / uzlib

**Date:** 2026-08-17 · **Branch at time of writing:** `codex/silabs-c13` · **Status:** analysis only,
no source modified.

Scope: the two in-tree uzlib copies, the second inflate backend on ESP32, the window-size wire
contract, the engine seam, the destination question, and how all of that relates to
`plans/PLAN_MIGRATION_ENDGAME_2026-08-17.md` § 2.3 row 1.

---

## 0. Summary of findings

1. **The two copies are byte-identical in every source file.** There is no version skew, no
   one-sided OD patch, and no target adaptation between them. The only difference is three
   PlatformIO-era metadata files present on one side. § 1.
2. **All five copies in the workspace are byte-identical too** — `../Firmware/lib/uzlib/`,
   `../Firmware_NRF54/third_party/uzlib/`, `../Firmware_Silabs/third_party/uzlib/` and both copies
   here agree on `od_zlib_stream.c`, `uzlib.h` and `uzlib_conf.h`. There is no upstream to diff
   against; the de-dup is unusually cheap. § 1.2.
3. **`adler32.c` and `crc32.c` are dead code.** `uzlib_adler32()` and `uzlib_crc32()` have no caller
   anywhere in this repo or in `../Firmware`, and only the ESP32 build compiles them. § 1.3.
4. **Two documented claims about the window are stale and should be corrected while here.** No
   build config in this repo *or* in upstream `../Firmware` sets `OPENDISPLAY_ZLIB_WINDOW_BITS` at
   all — every occurrence outside the header default is commented out, and the `esp32-s3-E1004`
   env that `../CLAUDE.md` and `docs/DIVERGENCE_MATRIX.md:224` name **does not exist**. The
   effective window is 9 bits / 512 bytes on every board of every target of every repo. § 2.1.
5. **A live wire divergence exists between the two ESP32 backends.** uzlib refuses any stream whose
   CMF declares more than 9 bits; tinfl refuses only above `OD_TINFL_DICT_SIZE` = 4096 = **12
   bits**. PSRAM/WiFi ESP32 boards therefore accept 10-, 11- and 12-bit streams that every other
   board in the fleet rejects. No shipping client hits it because `py-opendisplay` pins 9, but it is
   a real divergence and the shared pump should close it. § 2.3.
6. **`OPENDISPLAY_ZLIB_USE_HEAP_WINDOW` should be deleted, not promoted.** It is the only heap
   dependency in the engine, it buys 512 bytes of `.bss` on a 512 KB part, it is set on all ten
   ESP32 boards of which eight do not even call uzlib, and its failure path bricks the inflater for
   the rest of the boot. It cannot exist under `shared/`'s no-heap rule and it does not earn an
   exception. § 2.4.
7. **The seam needs no vtable and no new mechanism.** Five link-time `extern "C"` functions —
   exactly decision 1's boundary shape, and exactly what `od_inflate_tinfl.h:69-73` already declares
   — plus one file-level `#if` guard on the portable engine, which is the pattern
   `od_inflate_tinfl.cpp:11` already uses. § 3.
8. **The de-dup splits cleanly into a phase that needs no decision and a phase that is the roadmap
   item.** Phase 0 (collapse the two copies, delete the dead checksums) is provably behaviour-free
   and does not touch `shared/`, so it does not need the plain-C re-argument. Phase 1 is § 2.3
   row 1 and does. § 4.
9. **Recommendation: stay in plain C**, and the reason is specific rather than general — the
   "nested resource lifetimes, several failure exits" shape decision 1 reserves for re-argument
   **does not occur in this file** once the heap window is dropped. § 4.2.
10. **There is no host test for the inflater in `tests/host/`.** The only harness,
    `targets/esp32-idf/tools/test_zlib_stream.c`, is standalone and wired into nothing. § 6.

---

## 1. Diffing the two uzlib copies

### 1.1 In-tree: identical

`third_party/uzlib/src/` and `targets/esp32-idf/lib/uzlib/src/` contain the same five files. All
five are byte-identical:

```
3ab152c12679506e8c8e4eea1091eae7  {both}/uzlib_conf.h
45b73e295ac1944503328ff95c0f6de2  {both}/uzlib.h
5fa535a886edf27475df603222ebfdbe  {both}/adler32.c
6190cf7c78a383aaa18ec02fe97370dc  {both}/od_zlib_stream.c
6afc573e8a7a3ffd0910705edda2a7f4  {both}/crc32.c
```

`diff -rq` reports exactly three differences, all metadata and all one-sided — present in
`targets/esp32-idf/lib/uzlib/` and absent from `third_party/uzlib/`:

| File | Classification | Note |
|---|---|---|
| `targets/esp32-idf/lib/uzlib/LICENSE` | provenance | zlib licence text. **`third_party/uzlib/` has no LICENSE file at all.** |
| `targets/esp32-idf/lib/uzlib/README.md` | provenance | upstream uzlib readme |
| `targets/esp32-idf/lib/uzlib/library.json` | **PlatformIO residue** | `"name": "uzlib", "version": "0.0.0-opendisplay"`, `build.srcDir` — a PlatformIO `lib/` manifest, and CLAUDE.md decision 5 bans PlatformIO idioms. Dead under CMake. |

**There are no `OD-PATCH` markers in either copy.** A repo-wide grep finds seven `OD-PATCH` sites,
all in `third_party/bb_epaper`, `third_party/FastEPD` and the three panel glue TUs — none in
either uzlib tree.

**There is no upstream version skew, because `od_zlib_stream.c` is not upstream uzlib.** The
vendored header says so: "The original uzlib one-shot/callback inflater API is intentionally not
exposed in this vendored copy. Firmware uses one global streaming inflater"
(`third_party/uzlib/src/uzlib.h:4-5`). What is genuinely third-party is the *algorithm and the
static tables* — `length_bits`/`length_base`/`dist_bits`/`dist_base`/`clcidx`
(`third_party/uzlib/src/od_zlib_stream.c:34-66`), lifted from tinf — plus `uzlib_conf.h`, which is
verbatim upstream ("Copyright (c) 2014-2018 by Paul Sokolovsky",
`third_party/uzlib/src/uzlib_conf.h:2-4`). The 723-line resumable state machine around them is OD
code. That matters for § 5.

`adler32.c` and `crc32.c` *are* verbatim upstream, and carry their own third-party copyright
("Copyright (c) 2003 by Joergen Ibsen / Jibz", `third_party/uzlib/src/adler32.c:4`).

### 1.2 Cross-repo: also identical

The sibling repos are the authority (CLAUDE.md § "Migration constraints"), so the diff was extended
to them. All three shared files agree across all five copies in the workspace:

```
Firmware/lib/uzlib/src/od_zlib_stream.c              6190cf7c78a383aaa18ec02fe97370dc
Firmware_NRF54/third_party/uzlib/src/od_zlib_stream.c 6190cf7c78a383aaa18ec02fe97370dc
Firmware_Silabs/third_party/uzlib/src/od_zlib_stream.c 6190cf7c78a383aaa18ec02fe97370dc
Firmware_Unified/third_party/uzlib/src/od_zlib_stream.c 6190cf7c78a383aaa18ec02fe97370dc
Firmware_Unified/targets/esp32-idf/lib/uzlib/src/od_zlib_stream.c 6190cf7c78a383aaa18ec02fe97370dc
```

`uzlib.h` and `uzlib_conf.h` likewise. `Firmware_NRF54` and `Firmware_Silabs` do not carry
`adler32.c`/`crc32.c` at all — only `Firmware` does, and this repo inherited both.

**Consequence: there is no drift-vs-adaptation judgement to make here**, which is the one thing
that usually makes a de-dup expensive. The engine has one behaviour on four chips today.

### 1.3 Dead code in the vendored tree

`uzlib_adler32()` and `uzlib_crc32()` are declared at `third_party/uzlib/src/uzlib.h:44-45` and
defined at `third_party/uzlib/src/adler32.c:44` and `third_party/uzlib/src/crc32.c:49`. A
repo-wide grep plus a grep over `../Firmware/src` and `../Firmware/lib` finds **no caller** — the
inflater computes its Adler-32 inline and incrementally
(`adler_update_byte()`, `third_party/uzlib/src/od_zlib_stream.c:313-321`), and config CRC-16 is
`od_config_tlv.c`'s.

Only the ESP32 build compiles them (`targets/esp32-idf/main/CMakeLists.txt:30-31`); Nordic
(`targets/nordic-zephyr/zephyr/CMakeLists.txt:251`) and BG22
(`targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:272`) each list `od_zlib_stream.c` alone.
They are dropped at link on ESP32 too, so this is source-tree clutter rather than footprint — but
it is also two files of third-party copyright carried for nothing.

`od_zlib_stream_output_count()` (`third_party/uzlib/src/uzlib.h:42`) is likewise unused by firmware:
the only caller is the standalone harness `targets/esp32-idf/tools/test_zlib_stream.c:87`. See § 3.4
for why that is nevertheless load-bearing for the seam.

### 1.4 Duplication that is *not* in the engine

The engine is duplicated twice; the **pump loop around it is duplicated five times**, and the five
are structurally the same function:

| Site | Sink |
|---|---|
| `targets/esp32-idf/src/display_service.cpp:3098-3141` | direct write |
| `targets/esp32-idf/src/display_service.cpp:3144-3165` | partial write |
| `targets/nordic-zephyr/src/opendisplay_display.cpp:457-482` | partial write |
| `targets/nordic-zephyr/src/opendisplay_display.cpp:834-867` | direct write |
| `targets/efr32bg22-slc/opendisplay_display.cpp:651-684` | direct write |

Every one is: `push()`; then loop `poll()` into a chunk buffer, feed the sink, `continue` on
`OUTPUT_READY`, `return !final` on `NEEDS_INPUT`, compare a byte counter against an expected total
on `DONE`, log-and-fail on `ERROR`. The BG22 copy adds one extra invariant check
(`s_written_bytes - before != bytes_out`, `targets/efr32bg22-slc/opendisplay_display.cpp:667-669`)
that ESP32 applies only on its gray4 arm (`display_service.cpp:3119-3122`) and Nordic not at all.
**That is the actual shared code in this de-dup** — the engine is already one file logically, the
pump is not.

---

## 2. The window-size wire contract

### 2.1 Current state, verified

`OPENDISPLAY_ZLIB_WINDOW_BITS` is defined in exactly one place with a default of 9 and a hard range
check:

```
third_party/uzlib/src/uzlib.h:21-29
  #ifndef OPENDISPLAY_ZLIB_WINDOW_BITS
  #define OPENDISPLAY_ZLIB_WINDOW_BITS 9
  #endif
  #if OPENDISPLAY_ZLIB_WINDOW_BITS < 9 || OPENDISPLAY_ZLIB_WINDOW_BITS > 15
  #error "OPENDISPLAY_ZLIB_WINDOW_BITS must be in range 9..15"
  #endif
  #define OPENDISPLAY_ZLIB_WINDOW_SIZE (1u << OPENDISPLAY_ZLIB_WINDOW_BITS)
```

Who sets it:

| Build config | `WINDOW_BITS` | `USE_HEAP_WINDOW` |
|---|---|---|
| `targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:391-392` | **9 (explicit)** | 0 |
| `targets/nordic-zephyr/zephyr/CMakeLists.txt:111` | *unset* → 9 | 0 |
| all 10 of `targets/esp32-idf/boards/*.cmake` | *unset* → 9 | **1** |

The per-board lines are `s3-n16r8.cmake:12`, `s3-n16r8-extuart.cmake:13`,
`s3-n16r8-extuart-debug.cmake:17`, `s3-n32r8.cmake:9`, `s3-n32r8-extuart.cmake:20`,
`s3-n32r8-extuart-debug.cmake:16`, `s3-n8r8.cmake:9`, `c3-n4.cmake:26`, `c3-n16.cmake:26`,
`c6-n4.cmake:31`, `esp32-n4.cmake:23`.

**No board sets `WINDOW_BITS`.** Upstream `../Firmware/platformio.ini` does not either: every one of
its eight occurrences is commented (`; -DOPENDISPLAY_ZLIB_WINDOW_BITS=15` at line 80,
`#-DOPENDISPLAY_ZLIB_WINDOW_BITS=15` at 203, 233, 284, 368, 389, 410, 506), and there is no
`esp32-s3-E1004` env — `../Firmware/platformio.ini:343` says outright "The Seeed reTerminal E1004
has no env of its own: it is the same hardware as esp32-s3-N32R8-extuart."

So both `../CLAUDE.md` ("Only `esp32-s3-E1004` sets `=15` (32 KB)") and
`docs/DIVERGENCE_MATRIX.md:224` ("only `env:esp32-s3-E1004` pins **15 (32 KB)**
(`platformio.ini:152`)") are **stale**. `platformio.ini:152` today is unrelated prose about
nRF52840 debug logging. Their *conclusion* — 9 bits everywhere, a wire contract not a tunable — is
right and is in fact stronger than stated: there is no exception at all. The stale half is
worth deleting so nobody sizes a shared buffer for 32 KB "because one board needs it".

### 2.2 Why it is a wire contract and not a buffer

Three independent pieces of evidence:

1. **The device refuses over-wide streams rather than degrading.**
   `third_party/uzlib/src/od_zlib_stream.c:641-644`:
   ```c
   if (((s.cmf >> 4) + 8u) > OPENDISPLAY_ZLIB_WINDOW_BITS) {
       set_error("zlib stream window exceeds firmware limit");
       return OD_ZLIB_STATUS_ERROR;
   }
   ```
   A host that encodes at 15 gets a hard failure, not a slow decode.
2. **The host has a constant named after the firmware flag, and pins it on every path.**
   `../py-opendisplay/src/opendisplay/encoding/compression.py:14`:
   `FIRMWARE_ZLIB_WINDOW_BITS = 9`, with the comment "uzlib is compiled with
   `OPENDISPLAY_ZLIB_WINDOW_BITS=9` and rejects zlib headers advertising more". Used at
   `../py-opendisplay/src/opendisplay/device.py:395`, `:1937` and `:2110`. Note the library's own
   *default* is `zlib.MAX_WBITS` (15) — the 9 is applied deliberately at each upload site, and
   `device.py:1931` even re-compresses data that arrived at the wrong window.
3. **The canonical protocol header bakes the size into the meaning of a capability bit.**
   `shared/protocol/opendisplay_structs.h:679`:
   ```
   #define OD_TRANSMISSION_MODE_STREAMING_DECOMPRESSION (1u << 0)
       /* @doc "streaming zlib inflate, 512-byte DEFLATE window (requires zip). ..." */
   ```
   That is the whole of the negotiation. **The window size is not interrogable** — bit 0 is a
   boolean whose documented meaning *is* 512 bytes. There is no field a host can read to discover
   a different value, and `zlib_window_bits()`
   (`../py-opendisplay/src/opendisplay/encoding/compression.py:58`) reads the CMF byte of the host's
   *own* output, not anything the device said.

This is structurally the same situation as `OD_CONFIG_MAX_SIZE` (CLAUDE.md decision 12): a
per-target value a host cannot query. The difference is that decision 12 accepted the divergence
and paid for it with a loud refusal; here the value is *already* uniform, and the only sane move is
to keep it uniform.

### 2.3 The divergence the promotion must close

`od_inflate_tinfl.cpp` does **not** enforce 9. Its bound is the dictionary it hands miniz:

```
targets/esp32-idf/src/od_inflate_tinfl.cpp:51-57
  #ifndef OD_TINFL_DICT_SIZE
  #  if OPENDISPLAY_ZLIB_WINDOW_BITS == 9
  #    define OD_TINFL_DICT_SIZE 4096u          /* staging headroom */
  #  else
  #    define OD_TINFL_DICT_SIZE OPENDISPLAY_ZLIB_WINDOW_SIZE
  #  endif
  #endif
```

`tinfl_decompress()` is called with `s_dict`/`OD_TINFL_DICT_SIZE` as the output ring
(`od_inflate_tinfl.cpp:166-169`), and miniz's zlib-header check rejects a declared window larger
than the ring — which the file's own comment states
(`targets/esp32-idf/src/od_inflate_tinfl.cpp:22-24`). At the default that ring is **4096**, so the
effective acceptance limit on a tinfl build is **12 bits, not 9**. `docs/DIVERGENCE_MATRIX.md:225`
asserts "both engines reject an over-wide CMF" — true, but at *different* widths, and the matrix
does not record the gap.

Practical impact today: none, because `py-opendisplay` never emits anything but 9. Impact if left:
a host developed against a PSRAM S3 could silently start emitting 12-bit streams and fail on every
other board in the fleet, including both BG22 and Nordic. **The shared pump must apply the CMF
check itself, before the engine sees the bytes**, so that the refusal width is a property of the
contract rather than of whichever engine a board happens to link.

### 2.4 The heap window, and BG22

```
third_party/uzlib/src/od_zlib_stream.c:11-17    default 0, must be 0 or 1
third_party/uzlib/src/od_zlib_stream.c:157-161  uint8_t *window   vs   uint8_t window[SIZE]
third_party/uzlib/src/od_zlib_stream.c:564-577  malloc() in od_zlib_stream_reset()
```

Answering the question directly: **no, this option cannot exist in `shared/` in its current form.**
`malloc()` is the one construct CLAUDE.md § "Memory sensitivity" names — "`shared/` must avoid …
unbounded heap, and assuming a heap exists". BG22 sets it to 0
(`targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:392`) and Nordic sets it to 0
(`targets/nordic-zephyr/zephyr/CMakeLists.txt:111`), so nothing is lost by removing it there.

The stronger point is that it should be removed *everywhere*, not preserved behind a target macro:

- **It buys 512 bytes.** At the universal 9-bit window the allocation is `1u << 9`. Trading 512
  bytes of `.bss` for 512 bytes of heap on a 512 KB ESP32-S3 is not a saving; the flag was
  presumably written when someone believed the window was 32 KB (§ 2.1's stale claim).
- **Eight of the ten boards that set it never call the code.** `display_service.cpp:50-54` remaps
  the call sites to tinfl on every `OPENDISPLAY_ENABLE_WIFI` build, so `od_zlib_stream.o` is
  dropped at link there (the file says so, `display_service.cpp:36-37`). Only `c3-n4`, `c3-n16`,
  `c6-n4` and `esp32-n4` actually run uzlib — and those are precisely the parts with **no PSRAM**,
  where internal DRAM is scarcest and a heap allocation is least attractive.
- **Its failure mode is a permanently broken inflater.** `od_zlib_stream.c:565-575`: if `malloc`
  returns NULL, `reset()` sets `initialized = true` and an error string and returns. The cached
  `s_window` stays NULL, so every subsequent `reset()` retries — but any transfer that starts in
  the meantime fails at `push()`. There is no `free()`, so the buffer is a deliberate permanent
  allocation; the only thing the heap adds over `.bss` is a way for it to fail.

**Recommendation: delete `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW` in phase 0**, before anything moves.
That removes `<stdlib.h>` (`od_zlib_stream.c:5`) and the only allocation from the engine, and makes
the file boundary-clean for `shared/` with no further work.

### 2.5 What the shared module must therefore do

1. **Own the constant, `#ifndef`-guarded, floor 9, ceiling 15**, in the contract header — the
   `OD_CONFIG_MAX_SIZE` shape from decision 12, minus the divergence: no target overrides it today
   and none should without a protocol revision to bit 0's documented meaning. A comment saying so
   belongs next to it, because the *reason* is `opendisplay_structs.h:679`, not a RAM budget.
2. **Apply the CMF refusal in engine-neutral code**, per § 2.3, so the acceptance width is one
   number for the fleet.
3. **`_Static_assert` the derived window size** — decision 1's first C-side rule ("`_Static_assert`
   every wire size … Zero cost, C99, unused today"). This is the first shared file with an obvious
   candidate.
4. **Declare no output buffer of its own.** The pump should take the caller's chunk buffer as
   `(uint8_t *chunk, size_t chunk_len)`. The three targets disagree — 2048 on ESP32
   (`targets/esp32-idf/src/display_service.h:10`), 256 on Nordic
   (`targets/nordic-zephyr/src/opendisplay_display.cpp:28`), 256 on BG22
   (`targets/efr32bg22-slc/opendisplay_display.cpp:22`) — and there is no reason for `shared/` to
   arbitrate a buffer it does not consume. Documented floor: 1 byte is correct (the pump loops), so
   assert something conservative like 64 purely to catch a mistake, and let each target keep its
   number.
5. **Keep the engine's static footprint a compile-time constant with a measured floor.** BG22's
   measured uzlib working set is 1676 B (`docs/DIVERGENCE_MATRIX.md:128`) against ~10.5 KB of heap
   and 32 KB total; tinfl's ~11 KB of tables can never be the shared default, which
   `docs/DIVERGENCE_MATRIX.md:225` already records.

---

## 3. Two backends, one seam

### 3.1 Constraints

- `shared/` must not name `tinfl`, `miniz` or `uzlib`. `tools/check.sh:83` already forbids
  `#include <miniz…>` under `shared/` **and states the intended split in the check's own comment**:
  "miniz.h is the ROM tinfl header — the inflate ENGINE belongs in shared/compress, the ROM binding
  does not." The seam below is that sentence made executable.
- No second vtable (decision 2 — `od_panel_ops` is the only one).
- `shared/` is plain C (decision 1); `od_inflate_tinfl.cpp` is C++.
- A target must not pay for an engine it does not use (decision 9).

### 3.2 The seam: five link-time functions, which already exist

Decision 1 says it outright: "Every interface across the boundary is a link-time `extern` C
function the target implements." The inflate backend is a HAL in everything but name, and the
repo has already proved the shape twice — `od_inflate_tinfl.h:69-73` declares exactly the five
functions with exactly `od_zlib_stream_*`'s signatures, and
`docs/DIVERGENCE_MATRIX.md:225` already records the intent ("a target-selected backend behind one
streaming API (reset/push/poll/error/count), which the tinfl adapter already proves by reusing
uzlib's status enum").

Proposed contract header, `shared/compress/od_inflate.h`:

```c
typedef enum { OD_INFLATE_NEEDS_INPUT = 0, OD_INFLATE_OUTPUT_READY = 1,
               OD_INFLATE_DONE = 2, OD_INFLATE_ERROR = -1 } od_inflate_status_t;

void                od_inflate_reset(uint32_t expected_output_size);
od_inflate_status_t od_inflate_push(const uint8_t *in, size_t len, bool final);
od_inflate_status_t od_inflate_poll(uint8_t *out, size_t cap, size_t *produced);
const char         *od_inflate_error(void);
uint32_t            od_inflate_output_count(void);
```

No function pointers, no registration, no runtime dispatch — the linker resolves it, which is the
same answer C11 gave for `od_cmd_app.h`, and it inherits the same property: a target that
implements nothing is a link error, not a silent fallback.

### 3.3 Engine selection without `shared/` naming an engine

Selection is a **build-system fact**, which `plans/PLAN_MIGRATION_ENDGAME_2026-08-17.md:116` already
assigns to the target ("engine selection, window size and storage" remain target-owned).

- The **portable engine** — today's `od_zlib_stream.c`, renamed to the neutral symbols — is the
  default implementation. It is compiled by every consumer of the aggregate, wrapped in a
  file-level `#if OD_INFLATE_ENGINE_PORTABLE` … `#endif` that defaults to 1. This is not a new
  mechanism: it is precisely what `od_inflate_tinfl.cpp:11`/`:203` does today ("Compiled to an
  empty TU when the gate is off").
- A **target that supplies its own engine** sets `-DOD_INFLATE_ENGINE_PORTABLE=0` on its compile
  line and adds its own TU. On ESP32 that is `targets/esp32-idf/src/od_inflate_tinfl.cpp` with its
  five functions renamed; it stays C++ in `targets/`, where C++ is allowed, and it already carries
  the `extern "C" { … }` block (`od_inflate_tinfl.cpp:94`, `:201`) that makes the link work.
- ESP32's existing `#define od_zlib_stream_reset od_inflate_tinfl_reset` remap block
  (`targets/esp32-idf/src/display_service.cpp:50-55`) **disappears**, which is the point: the
  choice moves from a macro in one `.cpp` to a flag in one board fragment, and stops being
  invisible to the other twenty call sites in that file.

The one wrinkle worth naming: `OD_INFLATE_ENGINE_PORTABLE=0` is set per-target while
`OPENDISPLAY_ENABLE_WIFI` is set per-board, so the ESP32 side needs the flag on the seven WiFi
boards rather than on the target. That is a straight translation of the existing
`OPENDISPLAY_USE_TINFL` derivation (`targets/esp32-idf/src/od_inflate_tinfl.h:54-60`) into the board
fragments, and it makes the "PSRAM ⇒ can spare 11 KB" reasoning explicit at the place that knows
about PSRAM instead of inferring it from a transport flag — which `od_inflate_tinfl.h:16-18` warns
about at length.

### 3.4 Rename `od_zlib_stream_*`, don't alias

Two reasons not to keep the current names and `#define` around them.

First, the existing remap is **incomplete**: `display_service.cpp:51-54` rebinds four of the five
functions and omits `od_zlib_stream_output_count`. It is harmless today only because no ESP32 call
site uses it (§ 1.3) — but a future caller on a tinfl build would silently read the *uzlib* engine's
counter, which is permanently 0 because that TU is not even linked. A rename removes the class of
bug; an alias preserves it.

Second, `uzlib.h` currently leaks into two headers that have no business knowing the engine —
`targets/esp32-idf/src/main.h:12` and `targets/esp32-idf/src/od_inflate_tinfl.h:46` — because the
status enum lives there. Moving the enum into `od_inflate.h` fixes both.

### 3.5 The pump, and the `shared/sources.cmake` tier

The five duplicated pump loops (§ 1.4) become one shared function taking a sink:

```c
typedef bool (*od_inflate_sink_fn)(void *ctx, const uint8_t *bytes, uint32_t len);
bool od_inflate_pump(const uint8_t *in, uint32_t len, bool final,
                     uint8_t *chunk, size_t chunk_len,
                     od_inflate_sink_fn sink, void *ctx);
```

A single function-pointer *argument* is not a vtable; decision 2 forbids a second `_ops` struct of
pointers, and this is the same shape `od_config_tlv`'s walk already uses. If even that is judged too
close to the line, the alternative is a named link-time hook (`od_inflate_app_sink()`) in the
`od_cmd_app.h` style — but that forces one sink per binary, and ESP32 needs two (direct and
partial), so the argument form is the right one.

**Tier: a new `OD_SHARED_SOURCES_HAL_INFLATE`.** It answers the tier question correctly — "what
must a consumer already have in order to link this source?" — namely an implementation of
`od_inflate.h`'s five functions. Membership:

```cmake
set(OD_SHARED_SOURCES_HAL_INFLATE
    "${CMAKE_CURRENT_LIST_DIR}/compress/od_inflate_pump.c"     # needs the five functions
    "${CMAKE_CURRENT_LIST_DIR}/compress/od_inflate_portable.c" # PROVIDES them, when enabled
)
```

Both files in one tier, and the tier in the aggregate. That keeps the one-list rule intact
(`shared/sources.cmake:14-16`: "do not replace this with a GLOB", "every source belongs to exactly
one tier", the aggregate is composed not hand-maintained), and it makes the host tests compile the
portable engine automatically — closing § 6's gap by construction rather than by remembering.

All three targets take this tier today. BG22 takes it as a fourth tier alongside PURE +
HAL_CRYPTO + HAL_RADIO + APP_SESSION; it already compiles the engine
(`targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:272`), so this is a repoint, not a new
dependency.

---

## 4. Relationship to the roadmap

### 4.1 It is partly the roadmap item, and the separable part is worth doing first

`plans/PLAN_MIGRATION_ENDGAME_2026-08-17.md:116` lists "Compression stream/backend" as § 2.3's first
gated transfer-promotion unit, and `:134-137` requires the plain-C decision to be recorded before
that unit starts. Two distinct pieces of work are tangled here:

**Phase 0 — de-duplication only. Not the roadmap item. Needs no language decision.**
Nothing enters `shared/`; no behaviour changes; the diff is provably a no-op because the files are
byte-identical (§ 1.1).

1. Delete `targets/esp32-idf/lib/uzlib/` entirely (`lib/` then holds only `README`).
2. Repoint `targets/esp32-idf/main/CMakeLists.txt:29` and `:156` at
   `third_party/uzlib/src/`. Delete lines `:30-31` (the dead checksums) with them.
3. Delete `third_party/uzlib/src/adler32.c` and `crc32.c` and their declarations at
   `third_party/uzlib/src/uzlib.h:44-45` — no caller exists (§ 1.3).
4. Add the missing `third_party/uzlib/LICENSE` (currently only the copy being deleted has one).
5. Delete `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW` and its eleven build-config sites (§ 2.4).
6. Correct the stale window claims in `../CLAUDE.md` and `docs/DIVERGENCE_MATRIX.md:129`, `:224`
   (§ 2.1).

Verification for phase 0 is `tools/check.sh --targets`; ESP32, Nordic and BG22 must all still build,
and the BG22 image size must be unchanged except for the heap-window removal (which is already 0
there, so: byte-identical).

**Phase 1 — the § 2.3 row-1 promotion. This *is* the roadmap item and is gated on the decision.**
The contract header, the engine rename, the pump, the CMF-refusal unification (§ 2.3), the tier, and
Gate 1 + Gate 2 on all three targets.

Doing phase 0 first is not gold-plating: it removes two of the five files, the dead-code question
and the heap question from phase 1's diff, so the promotion commit contains only the promotion. It
also satisfies the "one subsystem per swap, independently revertable" constraint better than one
combined change would.

### 4.2 The plain-C decision: keep plain C

Decision 1 reserves the re-argument for "nested resource lifetimes with several failure exits per
function … the one shape where manual cleanup reliably loses". **That shape is not present in this
file**, and that is the decisive argument rather than a general preference for C:

- **After phase 0 the engine holds no resource at all.** The window becomes `uint8_t
  window[OD_INFLATE_WINDOW_SIZE]` with static storage duration (`od_zlib_stream.c:160`); the state
  is one file-scope `static od_zlib_stream_state_t s` (`:172`). There is nothing to acquire,
  nothing to release, no ordering between releases, and therefore nothing for a destructor to do.
  The one allocation in the file today is the very thing § 2.4 recommends deleting.
- **Error handling is already a sink, not an unwind.** `set_error()` (`:177-180`) parks the machine
  in `ST_ERROR` and every entry point short-circuits on it (`:618`, `:711`). There are many failure
  *returns* but exactly one failure *state*. RAII buys nothing against a state machine whose error
  state is idempotent.
- **The engine is resumable by construction**, which is the opposite of the RAII case: it exists
  precisely so that no state spans a function call — every partial decode is representable in `s`.
  That is why it is 723 lines of flat switch, and why C++ would change the line count and not the
  structure.
- **The price is real and spread across code `shared/` does not own** — decision 1's own accounting:
  a doubled host gate, an `-fno-exceptions -fno-rtti -fno-threadsafe-statics` contract across three
  build systems, and an enforcement grep libstdc++'s transitive includes defeat. Paying that for a
  file with no destructors is a bad trade.
- **The C++ that exists stays where C++ is legal.** `od_inflate_tinfl.cpp` remains in
  `targets/esp32-idf/`, and § 3.2's seam needs nothing from it but five `extern "C"` symbols it
  already exports (`od_inflate_tinfl.cpp:94`).

**This does not settle `od_xfer_partial.c`.** Decision 1 names three files; two of them —
`od_xfer_partial.c` and `od_session.c` — have genuinely nested lifetimes (PSA key slots, panel
sessions, transfer contexts) and several failure exits each. The recommendation here is scoped to
compression, and § 2.3 row 3 should re-argue on its own evidence rather than inherit this. Recording
the decision per-file is also what `PLAN_MIGRATION_ENDGAME:134-137` literally asks for.

---

## 5. Destination: `third_party/` or `shared/compress/`?

Decision 13 exempts `third_party/` from the one rule and says "still one vendored copy for all
targets; do not move it into `shared/`". Applied piece by piece:

| Piece | Destination | Why |
|---|---|---|
| `uzlib_conf.h` | **delete** | Verbatim upstream (`third_party/uzlib/src/uzlib_conf.h:2-4`), but all three of its knobs are at their defaults and nothing else references it once `adler32.c`/`crc32.c` are gone. It is included solely by `uzlib.h:19`. |
| `adler32.c`, `crc32.c` | **delete** | Dead (§ 1.3). |
| `library.json`, `README.md` | **delete** | PlatformIO residue (decision 5) and upstream readme for an API this copy deliberately does not expose (`uzlib.h:4-5`). |
| The tinf **tables and algorithm** (`od_zlib_stream.c:34-66` and the decode logic) | see below | Genuinely third-party in origin. |
| The **resumable state machine** wrapped around them (the other ~660 lines) | see below | OD code. |
| The **contract header** (status enum + five prototypes + window constant) | **`shared/compress/od_inflate.h`** | Uncontroversial: it is our API, engine-neutral, and both the pump and every target need it. |
| The **pump** (§ 1.4, § 3.5) | **`shared/compress/od_inflate_pump.c`** | Pure duplication of our own logic across three targets. This is the de-dup. |
| The **ROM binding** (`od_inflate_tinfl.cpp`) | **stays `targets/esp32-idf/src/`** | `tools/check.sh:83` says so explicitly; it includes `miniz.h` and is C++. |

**The contested piece is the engine, and the recommendation is `shared/compress/od_inflate_portable.c`.**

The case for `third_party/`: the tables and the decode algorithm derive from tinf, and decision 13's
"one vendored copy for all targets" already describes the desired end state — one copy, selected
per-target by the build system. Keeping it there makes phase 0 a two-file delete and phase 1 much
smaller.

The case for `shared/compress/`, which is stronger:

1. **`third_party/` is exempt from the boundary checks, and this file should not be.**
   `tools/check.sh:90-92` deliberately does not scan `third_party/`, for a reason that does not
   apply here — bb_epaper "selects its IO backend by `#ifdef` and every backend includes vendor
   headers, so it can never satisfy this rule". The inflater includes `<string.h>` and (after
   § 2.4) nothing else. It *can* satisfy the rule, and it should be held to it, because it is the
   file where a well-meaning `#include <esp_heap_caps.h>` for a PSRAM window would be most tempting.
2. **It is not vendored in the sense decision 13 protects.** Decision 13's property is "do not edit
   the vendored copy so a re-vendor stays a clean drop-in" — the property `OD-PATCH` markers exist
   to preserve. There is no upstream to re-vendor from: `uzlib.h:4-5` records that the upstream API
   was deliberately removed, and the file has been ours for long enough that all four repos carry
   the identical OD version (§ 1.2). Nobody will ever run `git subtree pull` on it.
3. **`shared/sources.cmake` is where the one-list rule lives.** In `third_party/`, three build files
   list the engine independently today (`main/CMakeLists.txt:29`,
   `zephyr/CMakeLists.txt:251`, `opendisplay-bg22.cmake:272`) — exactly the drift the single list
   exists to prevent, and exactly why the ESP32 copy diverged into a second tree in the first place.
4. **The host tests only see `shared/`.** `tests/host/` builds `OD_SHARED_SOURCES`; it has no
   `third_party/` path. Putting the engine in a tier is what gets it compiled under gcc + clang +
   ASan/UBSan and fuzzed alongside the rest, which § 6 says it never has been.
5. **`main/CMakeLists.txt:25-27` already declares the intent** — "od_zlib_stream.c is destined for
   `shared/compress/` at migration step 3-4 … compiled from `lib/` for now so that the promotion is
   a separate, revertable commit". So does `shared/sources.cmake:78`, whose order-of-arrival list
   ends with `compress/od_zlib_stream.c`.

Attribution is the one thing to get right on the way: the tinf-derived tables and the Jibz/Sokolovsky
licence notices must travel with the code, as a header comment naming the origin plus the licence
text retained in-tree. That is a comment, not a directory.

**Net result: `third_party/uzlib/` ceases to exist.** Everything in it is either deleted (§ 5 rows
1-3) or promoted. That is the cleanest possible answer to "two copies" — zero.

---

## 6. Test coverage (a gap, stated because the promotion should close it)

`tests/host/` has 30-odd test binaries and **not one touches inflate**. The only harness is
`targets/esp32-idf/tools/test_zlib_stream.c` (215 lines, differential fragmentation/chunking tests
against a reference), and it is referenced by three *documents*
(`docs/CORRECTNESS_REVIEW_2026-08-04.md:434`, `docs/TEST_OWNERSHIP.md:97`,
`docs/DESIGN_REVIEW_2026-07-25.md:134`) and by no build file at all. `docs/DESIGN_REVIEW_2026-07-25.md:134`
notes it "was never generalized".

This is a pre-auth-adjacent parser — it consumes attacker-controlled bytes during a transfer — and
`tools/check.sh` already runs pre-auth fuzz targets for other parsers. Phase 1 should port
`test_zlib_stream.c` into `tests/host/` alongside the promotion (CLAUDE.md § "Migration
constraints": "`../Firmware/tools/` also carries host tests worth porting with the code they cover"),
and add a corpus vector pair proving the CMF refusal at 10 bits — which is the check § 2.3 says the
two engines currently disagree about.

---

## 7. Open questions, ranked

1. **Is the 12-bit acceptance width on tinfl builds (§ 2.3) a defect to fix or a divergence to
   record?** Fixing it means the shared pump refuses `>9` before the engine runs, which is one
   `if`. Recording it means `DIVERGENCE_MATRIX.md:225` gains a row and the fleet keeps two
   acceptance widths. I recommend fixing, but it is a wire-behaviour change on a hardware-verified
   target and belongs to whoever owns § 2.3's Gate 2, not to this analysis. **Unverified:** the
   miniz-side check was read from `od_inflate_tinfl.cpp:22-24`'s comment and from the call's
   parameters (`:166-169`); the ROM `tinfl_decompress` source was not read.
2. **Does deleting `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW` need a board-level sign-off?** § 2.4 argues the
   flag is a 512-byte no-op with a failure mode, and the four boards that actually run uzlib are the
   no-PSRAM ones. But it is set on all ten ESP32 boards and on eleven build-config lines, and
   removing it changes `.bss` on parts where internal DRAM is the scarcest resource
   (`od_inflate_tinfl.h:22-24`). +512 B of DRAM on a C3-N4 is small but not nothing.
3. **`third_party/uzlib/` or `shared/compress/` for the engine?** § 5 recommends `shared/compress/`
   and gives five reasons, but it is the one recommendation here that turns on a judgement — how
   "vendored" a file with third-party tables and OD structure is — rather than on evidence. Decide
   before phase 1 starts; phase 0 is unaffected either way.
4. **Does the pump's sink argument (§ 3.5) sit on the right side of decision 2?** A `typedef`'d
   function-pointer parameter is not an `_ops` table, but the decision's spirit is "one vtable" and
   a reviewer could read it either way. The named-hook alternative is stated; it costs ESP32 its
   second sink.
5. **Should `OD_INFLATE_WINDOW_BITS` be `#ifndef`-guarded at all?** § 2.5 proposes the
   `OD_CONFIG_MAX_SIZE` shape for symmetry, but unlike that constant there is currently no target
   that overrides it and no mechanism for a host to discover an override
   (`opendisplay_structs.h:679` documents 512 bytes as the *meaning* of the capability bit). A hard
   `#define` with no escape hatch may be the more honest encoding of the contract.
6. **What happens to `targets/esp32-idf/tools/test_zlib_stream.c`?** Port into `tests/host/`
   (recommended, § 6), leave standalone, or delete. It is the only inflate test that exists.
7. **Is `uzlib_conf.h` safe to delete?** § 5 says yes — all three knobs are at defaults and only
   `uzlib.h:19` includes it. **Unverified** that no vendored SDK header elsewhere expects the
   `UZLIB_CONF_*` names; a grep found none, but grep does not see conditional compilation in the
   Simplicity SDK tree.
