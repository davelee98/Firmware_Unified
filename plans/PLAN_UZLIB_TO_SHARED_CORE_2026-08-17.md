# C14 — Canonical shared zlib inflater

**Status:** implemented in software on `c14-canonical-zlib-inflater`, 2026-08-18. Automated gate
complete; hardware matrix remains explicit debt. Scope stayed deliberately narrow — **the engine
moved, the pump did not.**

**Source:** [DEDUP_5_COMPRESSION_2026-08-17.md](DEDUP_5_COMPRESSION_2026-08-17.md).

**Relationship to the roadmap:** this is *not*
[PLAN_MIGRATION_ENDGAME_2026-08-17.md](PLAN_MIGRATION_ENDGAME_2026-08-17.md) § 2.3 row 1. That row
promotes the pump duplicated across the targets and settles engine selection. C14 only produces one
tested canonical portable engine, so the pump work starts from one implementation instead of two.

Licence considerations are out of scope by direction. Existing licence/provenance handling is not
an acceptance criterion for this plan.

## 1. Decisions

1. **Uzlib leaves `third_party/` entirely.** The directory is gone at the end of C14.
2. **The dead upstream files are deleted:** `adler32.c` and `crc32.c`. The live inflater performs
   its zlib Adler-32 validation internally; neither helper has a caller.
3. **The destination is `shared/core/`, not `shared/compress/`:**

   ```text
   shared/core/od_zlib_inflate.c
   shared/core/od_zlib_inflate.h
   ```

   The placeholder-only `shared/compress/` directory is then deleted.
4. **The engine is `PURE`.** After heap-window removal it depends only on the C standard library.
   `HAL_INFLATE` belongs to the later pump/engine-selection seam, not this source file.
5. **Plain C remains the implementation language.** Removing the heap leaves one static state
   object and no nested resource lifetime to unwind.
6. **Characterisation tests precede behavioural edits.** The existing standalone harness becomes
   a required host suite before the heap/state/header rewrite.
7. **C14 lands separately from the pump migration.** It creates a tested source and API baseline;
   the later engine-selection work can change that seam without also moving the implementation.
8. **The canonical filenames are `od_zlib_inflate.c` and `od_zlib_inflate.h`.** They identify both
   the zlib wrapper format and the inflate operation. C14 retains the existing
   `od_zlib_stream_*` function names and status type to keep the move behaviour-neutral; any public
   API rename belongs with the later engine-selection seam.

## 2. Why this is not just a directory move

`third_party/uzlib/` is mostly OpenDisplay code:

| File | Current role | C14 result |
|---|---|---|
| `od_zlib_stream.c` | OD's 723-line resumable inflater derived from uzlib/tinf | renamed to canonical `shared/core/od_zlib_inflate.c` |
| `uzlib.h` | OD streaming API and window contract | folded and renamed to `od_zlib_inflate.h` |
| `uzlib_conf.h` | configuration header | live definitions folded; file deleted |
| `adler32.c` | unused upstream helper | deleted |
| `crc32.c` | unused upstream helper | deleted |

The five source files are byte-identical between `third_party/uzlib/src/` and
`targets/esp32-idf/lib/uzlib/src/`. There is no divergent implementation to reconcile.

## 3. Pre-C14 consumers and backend split

| Consumer | Current portable-engine wiring |
|---|---|
| `nordic-zephyr` | explicit third-party include directory and source entry |
| `efr32bg22-slc` | explicit third-party include directory and source entry; window fixed at 9 |
| `esp32-idf` | explicit source and include entries from its target-local copy |
| `tests/host` | no integrated inflater suite; only the standalone ESP32 tool |

There are **11 ESP32 board configurations**. Seven Wi-Fi S3 configurations select the tinfl
adapter; four configurations (`c3-n4`, `c3-n16`, `c6-n4`, and `esp32-n4`) call the portable
inflater. Every board currently defines `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1`. Nordic and Silabs
define it as zero.

Adding `od_zlib_inflate.c` to `OD_SHARED_SOURCES_PURE` automatically supplies it to all three target
builds: ESP32 consumes the aggregate, while Nordic and Silabs already consume the PURE tier.
Therefore the final move **removes** each target's explicit inflater source entry; it does not
repoint those entries to `shared/core`.

ESP32 retains its second backend, `src/od_inflate_tinfl.cpp`, selected by the existing compile-time
symbol remap. C14 does not change backend selection or reconcile its wider effective window.

## 4. Commit sequence

Each commit builds independently. Tests become load-bearing before state or API changes.

### C14.1 — collapse the ESP32 duplicate

- Delete `targets/esp32-idf/lib/uzlib/`.
- Point ESP32's temporary explicit source/include entries at `third_party/uzlib/src/`.
- Update ESP32's active path-specific documentation and source/build comments in the same commit:
  references to the deleted `lib/uzlib` path must either name the temporary third-party location or
  become path-neutral. This includes `targets/esp32-idf/README.md`, its tools README, the main CMake
  comment, and the tinfl/display-service comments. Do not leave active documentation knowingly
  wrong until the final move.
- Build all **11** ESP32 board configurations.
- Prove the source-path-only change is behaviour-free using one of:
  - pre/post application binaries built with the same fixed `SOURCE_DATE_EPOCH`; or
  - the stripped inflater object's `.text`/`.rodata` when build-time metadata prevents a full-image
    comparison.

Do not claim an ordinary `.bin` comparison is deterministic: the current ESP-IDF configurations
enable compile-time date metadata.

### C14.2 — integrate the current inflater tests

Promote `targets/esp32-idf/tools/test_zlib_stream.c` into `tests/host/` while it still exercises the
unmodified implementation. Compile the same production source and harness as **two separate test
executables**:

- `od_zlib_inflate_static_test`, with `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=0`; and
- `od_zlib_inflate_heap_test`, with `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1`.

The first pins the Nordic/Silabs path and the second pins the ESP32 path that C14.4 removes. They
must run the same case table and produce identical results. Do not link both variants into one
executable: the production source has global API symbols and singleton state. The test executables
may compile the third-party source directly for this commit; the surviving static test switches to
`od_shared` after the final move. If fixtures are generated with the host zlib library, make that
dependency required rather than silently skipping the suite.

Required cases:

- stored, fixed-Huffman, and dynamic-Huffman streams;
- input/output chunk sizes of one byte, uneven chunks, and whole-buffer operation;
- valid 9-bit-CMF stream accepted;
- valid 10-bit-CMF stream rejected specifically as an oversized window;
- truncated input rejected after final input;
- corrupt **Adler-32 trailer** rejected specifically as an Adler mismatch;
- trailing input rejected;
- expected output size too small and too large rejected;
- `od_zlib_stream_output_count()` agrees with bytes emitted on success and error paths.

Negative tests assert the expected diagnostic or failure class, not merely that some error occurred.
Register both variants with CTest so gcc, clang, ASan, and UBSan runs in `tools/check.sh` execute
both. The heap variant is intentionally retired in C14.4 after its results have established parity
with the static variant.

### C14.3 — delete dead checksum helpers

- Delete `adler32.c` and `crc32.c` from `third_party/uzlib/src/`.
- Remove their two temporary ESP32 source entries.
- Remove the corresponding dead `uzlib_adler32()` and `uzlib_crc32()` declarations from
  `uzlib.h` in the same commit. A buildable intermediate header must not advertise functions whose
  definitions have been deleted.
- Run the complete host gate and all target builds.
- Before C14.4 changes storage, capture the comparison baseline from one portable-engine ESP32
  board: `.bss`, total DRAM, flash, and the relevant inflater symbols/sections from its link map.
  Capture a Wi-Fi S3 link map at the same commit to show the pre-change tinfl disposition. Preserve
  the exact board names and build profile with the recorded numbers so C14.4 can reproduce them.

The zlib wrapper's Adler-32 trailer verification in `od_zlib_stream.c` remains unchanged and is
pinned by C14.2.

### C14.4 — remove heap mode and establish the final header

- Remove `<stdlib.h>`, `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW`, its conditional branches, allocation,
  and permanent-failure path.
- Store the 512-byte history window directly in the static state object.
- Remove the heap-window definition from:
  - all 11 ESP32 board fragments;
  - Nordic's CMake configuration; and
  - Silabs' CMake configuration.
- Remove `od_zlib_inflate_heap_test` from the host CMake registration in this commit. Rename the
  surviving static variant to `od_zlib_inflate_test` (or its final CTest name) so no permanent test
  name implies a selectable storage mode that no longer exists.
- Fold the live API/configuration definitions from `uzlib.h` and `uzlib_conf.h` into
  `od_zlib_inflate.h`.
- Delete `uzlib.h` and `uzlib_conf.h` in this same commit after the fold. The third-party source
  directory remains on target include paths temporarily because `od_zlib_inflate.h` still lives
  there until C14.5; only the obsolete header files disappear here.
- In the same commit, replace every `#include "uzlib.h"` with
  `#include "od_zlib_inflate.h"`, including the portable source, all target consumers, the ESP32
  tinfl adapter, and host tests. Update nearby API/path comments at the same time.
- Retain `OPENDISPLAY_ZLIB_WINDOW_BITS`, its 9-bit default, and its 9..15 compile-time range check.

Verification:

- complete host gate and all target builds;
- CTest contains only the surviving static-window inflater test; the heap variant no longer builds
  because both the option and implementation have been removed;
- `rg` proves `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW`, `#include "uzlib.h"`, `uzlib.h`, and
  `uzlib_conf.h` are absent from active source/build inputs;
- rebuild the exact portable-engine ESP32 board/profile recorded in C14.3 and report its `.bss`,
  total DRAM, and flash deltas against that baseline;
- rebuild the same Wi-Fi S3 profile and compare its link map with the C14.3 baseline, proving the
  portable inflater's state remains discarded when tinfl owns the call sites.

### C14.5 — move and rename into `shared/core`

- Move and rename `third_party/uzlib/src/od_zlib_stream.c` to
  `shared/core/od_zlib_inflate.c`, and move `od_zlib_inflate.h` beside it as
  `shared/core/od_zlib_inflate.h`.
- Change the source's own include to `#include "od_zlib_inflate.h"` as part of that move.
- Add `core/od_zlib_inflate.c` exactly once, to `OD_SHARED_SOURCES_PURE`.
- Remove the explicit inflater source and obsolete uzlib include directory from Nordic, Silabs,
  and ESP32. Do **not** add a replacement target-specific source path: each target already consumes
  PURE.
- Link the host inflater test through `od_shared` rather than compiling a private source copy.
- Delete `third_party/uzlib/` and the now-tenantless `shared/compress/`.
- Correct the `shared/sources.cmake` arrival comment.
- Update active documentation and build comments that still prescribe `shared/compress`,
  `third_party/uzlib`, `lib/uzlib`, `uzlib.h`, `uzlib_conf.h`, the old source filename
  `od_zlib_stream.c`, or heap-window selection. This includes at least:
  - `docs/ARCHITECTURE.md`;
  - `docs/SHARED_API_DESIGN.md`;
  - `docs/TOOLCHAINS.md`;
  - `docs/MEMORY_CONSTRAINTS.md`;
  - `docs/DIVERGENCE_MATRIX.md`;
  - `docs/TEST_OWNERSHIP.md`;
  - target and tools READMEs; and
  - `tools/check.sh` comments.

An `rg` ratchet must find no stale active file/path reference. It must distinguish the obsolete
filename `od_zlib_stream.c` from the deliberately retained `od_zlib_stream_*` API symbols.
Historical plans, audits, and findings may retain old paths only when clearly describing prior
state.

## 5. Final acceptance gate

### Automated

- `tools/check.sh` passes under gcc, clang, ASan, and UBSan with the inflater suite enabled.
- `tools/check.sh --targets` passes.
- All 11 ESP32 board configurations build.
- Nordic and Silabs target builds pass.
- Link-map inspection proves:
  - a portable-engine ESP32 build contains the shared inflater and its static state; and
  - a tinfl ESP32 build does not retain that unused state.

### Hardware

Run an end-to-end compressed image upload on:

1. Nordic;
2. EFR32BG22;
3. one ESP32 configuration that actually uses the portable engine (`c3-n4`, `c3-n16`, `c6-n4`,
   or `esp32-n4`); and
4. one Wi-Fi S3 configuration that uses tinfl, proving the renamed shared header and adapter seam
   did not break that backend.

A board family that was not run remains explicit hardware debt; a green host/target build is not
reported as hardware verification.

## 6. Explicitly deferred work

- Promote the five duplicated pump loops.
- Build the engine-selection seam.
- Resolve D9: the portable engine rejects CMF windows wider than 9 bits while ESP32 tinfl currently
  has a wider effective dictionary.
- Change `od_inflate_tinfl.cpp` or its C++ implementation language.

## 7. Remaining implementation risks

- **Unremapped output count:** ESP32 remaps four of the five streaming symbols. During C14.4/C14.5,
  use target link maps or `nm` to prove `od_zlib_stream_output_count` remains unreferenced in tinfl
  builds and cannot pull in portable state. Fixing the engine seam remains deferred.
- **Header rename ordering:** C14.4 creates `od_zlib_inflate.h`, replaces all includes, and deletes
  `uzlib.h`/`uzlib_conf.h` atomically. The temporary third-party include directory remains until
  C14.5 moves the new header into `shared/core`; C14.5 then removes that directory from every target.
- **Static-state footprint:** heap removal changes where the 512-byte window is accounted. Preserve
  the recorded portable-board memory delta with the plan results.
- **Backend-blind testing:** an S3-only hardware pass exercises tinfl and says nothing about the
  relocated portable engine; the four-device matrix above is mandatory.

## 8. Implementation record

The five-stage sequence landed without combining the deferred pump/backend-selection work:

- `0bd6092` — removed ESP32's byte-identical target-local uzlib tree. Git blob identity proved the
  live source itself was unchanged, and all 11 ESP32 configurations built.
- `5fb3f5c` — moved the characterization harness into `tests/host/`. Static and heap variants both
  passed the same stored/fixed/dynamic, chunking, CMF-window, truncation, Adler, trailing-input,
  expected-size, and output-count cases. Fixtures are independently generated and committed, so
  this suite does not silently disappear when host zlib development files are absent.
- `9386e68` — removed the unused Adler-32 and CRC-32 helper translation units and declarations.
- `b25654f` — removed heap-window selection, allocation, and obsolete headers; all target families
  compiled the final static-state form before it moved.
- C14.5 moved `od_zlib_inflate.{c,h}` into `shared/core`, registered the source once in PURE,
  removed every target-specific source/include entry, and linked the permanent host test through
  `od_shared`. The old `third_party/uzlib/` and placeholder `shared/compress/` trees have no files.

Measured on the portable-engine `c3-n4` profile, C14.3 → final:

| Metric | C14.3 baseline | Final | Delta |
|---|---:|---:|---:|
| Flash code | 453,858 B | 453,790 B | -68 B |
| DRAM total | 155,968 B | 156,464 B | +496 B |
| `.bss` | 62,016 B | 62,512 B | +496 B |
| `.data` | 10,420 B | 10,420 B | 0 B |
| Flash data | 140,924 B | 140,884 B | -40 B |
| Total image | 688,800 B | 688,692 B | -108 B |

The +496 B static-RAM delta is the intended relocation of the 512-byte history window from heap
allocation into the singleton state, net of removed heap bookkeeping. The final `c3-n4` link map
contains `od_zlib_inflate.c`, the four live stream entry points, and a 0x694-byte state section.
The `s3-n16r8` tinfl profile contains none of the portable inflater object, entry points, or state;
`od_zlib_stream_output_count()` remains unreferenced and does not pull it in.

Final automated verification: `tools/check.sh --targets` reports **16 passed, 0 failed, 0
skipped**. That includes gcc, clang, ASan+UBSan, three 60-second pre-auth fuzz runs, the pinned
Python wire-corpus replay, all 11 ESP32 configurations plus sdkconfig baselines, all three Nordic
boards, and the BG22 headless build. The permanent host suite registers one static inflater test
and passes 35/35 in a direct gcc run.

No C14 hardware case has been run. The four-device compressed-upload matrix in §5 remains debt;
software/build verification is not substituted for it.
