# DEDUP 2 — the QR code encoder

Analysis only. No source file was modified. 2026-08-17.

## Summary

Three copies of a QR encoder, ~529 lines each, one per target. **They are not divergent.** The
ESP32 copy differs from the other two by brace style alone — proven by a differential test, not by
reading the diff. There is no user-visible defect today.

The decision this analysis exists to make is **provenance**, and it resolves cleanly: this is
vendored MIT third-party code (ricmoo/QRCode, © 2017 Richard Moore and Project Nayuki), which puts
it in **`third_party/`**, not `shared/core/`. It takes **no tier** in `shared/sources.cmake`.

Two real defects surfaced that are independent of de-duplication:

1. **The MIT copyright notice has been stripped from all six files.** MIT requires it be retained.
2. **A local edit replaced upstream's derived buffer size with a fixed 407-byte array**, costing
   464 bytes of stack at the version every target actually uses — against a BG22 main stack of
   2,752 bytes.

---

## 1. Exactly how the ESP32 copy diverged

### 1.1 Inventory, verified and extended

The inventory in the task brief is correct, and the divergence extends to a **fourth** variant in
`../Firmware_NRF` that the brief did not list.

| Copy | Lines | md5 (`.c`) | Identical sibling |
|---|---|---|---|
| `targets/esp32-idf/src/qr/qrcode.c` | 377 | `d8aecc…` | `../Firmware/src/qr/qrcode.c` (byte-identical) |
| `targets/nordic-zephyr/src/qr/qrcode.c` | 529 | `6c8dc1…` | `../Firmware_NRF54/src/qr/qrcode.c` (byte-identical) |
| `targets/efr32bg22-slc/qr/qrcode.c` | 529 | `6c8dc1…` | `../Firmware_Silabs/qr/qrcode.c` (byte-identical) |
| `../Firmware_NRF/qr/qrcode.c` (not imported) | 405 | `494516…` | — |

Headers: `targets/esp32-idf/src/qr/qrcode.h` (33 lines, `ddf5bd…`) versus
`targets/nordic-zephyr/src/qr/qrcode.h` = `targets/efr32bg22-slc/qr/qrcode.h` (46 lines,
`637604…`). No other files exist in any `qr/` directory — no build fragment, no test, no licence
file.

**No post-import drift.** Every imported copy is byte-identical to its sibling repo of origin, and
`git log` shows each arriving in exactly one import commit and never being touched since:

```
3687447 import(efr32bg22-slc): EFR32BG22 sources unchanged from Firmware_Silabs@17a8222
379b19e import(nordic-zephyr): nRF54L15 sources unchanged from Firmware_NRF54@0f19c0c
17557bd import(esp32-idf): Phase A - Firmware sources unchanged, does not build
```

So the sibling-versus-snapshot hazard in CLAUDE.md § "Migration constraints" does not apply here:
for this file the snapshots and the live repos agree.

### 1.2 Classification of every hunk: all of it is brace style

A comment-stripped, whitespace-normalised diff of the ESP32 copy against the Nordic/Silabs copy
yields **three** classes of change and nothing else:

| Class | What | Verdict |
|---|---|---|
| Brace expansion | ~45 single-statement `if`/`for` bodies written inline on ESP32, braced on Nordic. E.g. `targets/esp32-idf/src/qr/qrcode.c:17` `if (version > 9) modeInfo >>= 9;` versus `targets/nordic-zephyr/src/qr/qrcode.c:24-26`. | **Cosmetic** |
| Include order | `stdlib/string/limits` (`targets/esp32-idf/src/qr/qrcode.c:3-5`) versus `limits/stdlib/string` (`targets/nordic-zephyr/src/qr/qrcode.c:4-6`). All three are standard headers with no ordering dependency. | **Cosmetic** |
| Two spellings | `BitBucket codewords;` (`targets/esp32-idf/src/qr/qrcode.c:333`) versus `struct BitBucket codewords;` (`targets/nordic-zephyr/src/qr/qrcode.c:470`) — the type is a tagged typedef, both are legal and identical. And a named temporary before `return` in `qrcode_getDataCapacityBytes` (`targets/nordic-zephyr/src/qr/qrcode.c:443-444`) versus a direct return (`targets/esp32-idf/src/qr/qrcode.c:317`). | **Cosmetic** |

There is **no** upstream bug fix, **no** target adaptation, **no** accidental drift and **no** local
hack among them. The `../Firmware_NRF` variant falls into the same three classes.

### 1.3 Proof, not inspection

Reading a 500-line diff is not evidence. I compiled all three variants into one binary with their
five public symbols renamed, and compared outputs over 8 inputs × versions 1–10:

```
cases=65 failures=0
```

Compared per case: return code, `qr.size`, `qr.mode`, `qr.mask`, the full module bitmap
(`memcmp` over `qrcode_getBufferSize(v)` bytes), and every module via `qrcode_getModule()`. The
inputs included the real boot-screen URL shape, an empty string, a single character, high-bit
bytes, and an over-capacity string to exercise the rejection path.

**All three copies are byte-identical encoders.** The concern that a QR encoder producing different
output on different targets would be a user-visible defect is correct in principle and does not
apply here — the fleet is consistent. The cost of the duplication is maintenance and licence
exposure, not incorrect output.

### 1.4 Which copy is "correct"

The question is moot for behaviour — they agree. For everything else, **the Nordic/Silabs copy is
the better base**, on one ground only: its header retains an attribution line
(`targets/nordic-zephyr/src/qr/qrcode.h:1-4`, "Minimal QR header (MIT) derived from
ricmoo/QRCode"), and the ESP32 header (`targets/esp32-idf/src/qr/qrcode.h:1-3`, which opens with a
blank line and `#ifndef`) has **stripped even that**. That inverts the usual `Firmware`-is-authority
default in CLAUDE.md § "Migration constraints" — that default settles *algorithm* disputes, and
there is no algorithm dispute here.

---

## 2. Provenance — the central question

### 2.1 It is vendored, and the upstream is identified

`targets/nordic-zephyr/src/qr/qrcode.h:1-4` and `targets/efr32bg22-slc/qr/qrcode.h:1-4` both say:

```
/**
 * Minimal QR header (MIT) derived from ricmoo/QRCode.
 * Minimal QR encode APIs used by the OpenDisplay boot / draw tools.
 */
```

I located a pristine upstream copy on this machine at
`/mnt/c/SLC/EFR/Tag_FW_EFR32xG22/firmware/common/QRCode/` (876-line `src/qrcode.c`, 99-line
`src/qrcode.h`, `LICENSE.txt`, `library.properties`, `README.md`, `tests/`, `generate_table.py`).
Its header block is unambiguous:

```
 * The MIT License (MIT)
 * This library is written and maintained by Richard Moore.
 * Major parts were derived from Project Nayuki's library.
 * Copyright (c) 2017 Richard Moore     (https://github.com/ricmoo/QRCode)
 * Copyright (c) 2017 Project Nayuki    (https://www.nayuki.io/page/qr-code-generator-library)
```

`library.properties` gives `name=QRCode`, `version=0.0.1`, `url=https://github.com/ricmoo/qrcode/`.

**Provenance chain** (the last link is inference, flagged below): that directory is a git submodule
of `https://github.com/OpenEPaperLink/Tag_FW_EFR32xG22` (confirmed via
`git config --get remote.origin.url` in that tree). The OpenDisplay Silabs firmware descends from
that OpenEPaperLink tag firmware, which is the most likely route by which ricmoo/QRCode entered the
OpenDisplay codebase and then propagated to the other three firmware repos.

### 2.2 Our copies are a literal derivative, heavily reduced

Every internal symbol in our copies exists upstream with the same name and the same body:
`bb_appendBits`, `bb_getGridSizeBytes`, `bb_initBuffer`, `bb_initGrid`, `bb_setBit`, `bb_invertBit`,
`bb_getBit`, `applyMask`, `setFunctionModule`, `drawFinderPattern`, `drawAlignmentPattern`,
`drawFormatBits`, `drawFunctionPatterns`, `drawCodewords`, `getPenaltyScore`, `rs_multiply`,
`rs_init`, `rs_getRemainder`, `encodeDataCodewords`, `performErrorCorrection`, `getModeBits`.
Even the undocumented magic constant survives verbatim — `unsigned int modeInfo = 0x7bbb80a`
(`targets/nordic-zephyr/src/qr/qrcode.c:23`, upstream `src/qrcode.c:153`).

The reduction from upstream:

| Dimension | Upstream | Our fork |
|---|---|---|
| Versions | 1–40 | **1–10** (`targets/nordic-zephyr/src/qr/qrcode.c:431`) |
| ECC levels | L / M / Q / H | **M only** (`:450`, returns `-1` otherwise) |
| Encoding modes | numeric / alphanumeric / byte | **byte only** (`:372`, comment "we always encode URLs") |
| `LOCK_VERSION` compile-time specialisation | present | removed |
| Extra API | — | **`qrcode_getDataCapacityBytes()` added** (`:437`), not upstream |

Table extraction verified correct against upstream `src/qrcode.c:45-66`: our three 10-entry arrays
(`targets/nordic-zephyr/src/qr/qrcode.c:11-17`) are exactly columns 1–10 of upstream's Medium rows
and of `NUM_RAW_DATA_MODULES`.

The one *semantic* local edit versus upstream is correct: our copies hardcode
`uint8_t eccFormatBits = 0;` (`targets/nordic-zephyr/src/qr/qrcode.c:463`,
`targets/esp32-idf/src/qr/qrcode.c:328`) where upstream computes
`(ECC_FORMAT_BITS >> (2 * ecc)) & 0x03` (upstream `src/qrcode.c:786`). With
`ECC_FORMAT_BITS = 0xB1` (upstream `:769`) and `ecc == ECC_MEDIUM == 1`, that expression evaluates
to `0`. Hardcoding it is a valid consequence of dropping the other three ECC levels.

### 2.3 Destination: `third_party/`

This is not a close call. Modified-but-clearly-derivative MIT code with named upstream copyright
holders is exactly what CLAUDE.md decision 13 and `third_party/NOTICE.md` exist to govern. It goes
to `third_party/`, one copy for all targets, and it must **not** be moved into `shared/core/`.

That it would *technically* satisfy the one rule (see § 3) is not an argument for `shared/` —
`third_party/` is exempt from the one rule, so the rule cannot discriminate between the two. The
discriminator is authorship, and the authorship is Richard Moore's and Project Nayuki's.

Proposed shape, mirroring the existing `third_party/bb_epaper/` and `third_party/uzlib/`:

```
third_party/QRCode/
  LICENSE            <- verbatim from upstream LICENSE.txt (currently absent everywhere)
  src/qrcode.c       <- the reduced fork, MIT header block restored
  src/qrcode.h       <- likewise
```

with a new `## QRCode` section in `third_party/NOTICE.md` following the existing
Author / Upstream / Revision / Licence / Vendored / Scope table format, and recording the
reduction in § 2.2 as a local patch — the same way that file already records the bb_epaper and
FastEPD OD-PATCHes.

### 2.4 Licence compliance is currently broken

MIT: *"The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software."*

Measured across all six in-tree files:

| File | "MIT" | "Copyright" | Moore | Nayuki |
|---|---|---|---|---|
| `targets/esp32-idf/src/qr/qrcode.c` | 0 | 0 | 0 | 0 |
| `targets/esp32-idf/src/qr/qrcode.h` | 0 | 0 | 0 | 0 |
| `targets/nordic-zephyr/src/qr/qrcode.c` | 0 | 0 | 0 | 0 |
| `targets/nordic-zephyr/src/qr/qrcode.h` | 1 | **0** | 0 | 0 |
| `targets/efr32bg22-slc/qr/qrcode.c` | 0 | 0 | 0 | 0 |
| `targets/efr32bg22-slc/qr/qrcode.h` | 1 | **0** | 0 | 0 |

No file anywhere in the repo carries the copyright line, and `third_party/NOTICE.md` has no QR
entry (`grep -i qr` returns nothing). Two of the six name the licence without the notice; four name
neither.

**This is a defect that exists today and is worth fixing regardless of whether de-duplication ever
happens.** It is also the single cheapest argument for doing the de-duplication: one vendored copy
with a correct notice replaces four places to get it wrong.

---

## 3. The seam — there is none

**The encoder is pure computation over a caller-supplied buffer.** Plainly: this is the cheapest
de-duplication available in this repo.

Evidence:

- **Includes are `<stdlib.h>`, `<string.h>`, `<limits.h>` only**
  (`targets/nordic-zephyr/src/qr/qrcode.c:4-6`). No vendor header, no framework header, no
  `od_hal_*`. It would pass `boundary_includes` in `tools/check.sh:75` unmodified.
- **No allocation.** No `malloc`, no `free`, no heap of any kind. `stdlib.h` is pulled in for
  `abs()` (`:147`, `:160`) and nothing else.
- **No mutable static state.** `nm` on the compiled object reports zero `.data` and zero `.bss`;
  the only statics are three `const` tables in `.rodata` totalling 50 bytes
  (`NUM_ERROR_CORRECTION_CODEWORDS_M` 20 B, `NUM_ERROR_CORRECTION_BLOCKS_M` 10 B,
  `NUM_RAW_DATA_MODULES` 20 B). It is therefore reentrant.
- **The output buffer is the caller's.** `qrcode_initBytes(qrcode, modules, …)` writes the module
  bitmap into `modules`, and `qrcode_getBufferSize(version)` tells the caller how large that must
  be — the sizing decision is already the caller's, on every target.
- **No I/O, no time, no logging, no randomness.** Nothing that could require a HAL.
- **It is already plain C**, already `extern "C"`-guarded for the three C++ call sites, and already
  compiles under all three toolchains.

All three call sites use the identical API and the identical version:

| Target | Call site | Buffer |
|---|---|---|
| esp32-idf | `targets/esp32-idf/src/boot_screen.cpp:693-698` | `uint8_t qrBuf[256]` (stack), `qrVersion = 6` |
| nordic-zephyr | `targets/nordic-zephyr/src/boot_screen.cpp:725-730` | `uint8_t qrBuf[256]` (stack), `qrVersion = 6` |
| efr32bg22-slc | `targets/efr32bg22-slc/opendisplay_display.cpp:362-414` | `static uint8_t qr_buf[256]` (BSS), version `6u` |

The only per-target difference is *where the caller puts its 256-byte buffer* — BG22 makes it
`static` to keep it off a 2,752-byte stack. That is a call-site choice already, and de-duplication
does not touch it.

The encoder is used **only** by the boot screen on all three targets; nothing on the device
generates QR codes for uploaded content (ODL `qr` elements are rendered host-side by
`odl-renderer`). So the blast radius of any change here is the boot screen alone.

### Build wiring, if the file moves

Three edits, all mechanical:

- `targets/esp32-idf/main/CMakeLists.txt:19-23` — a `file(GLOB …)` that includes
  `${OD_SRC_DIR}/qr/*.c`. Once the directory is gone the glob silently matches nothing, so the new
  path must be added explicitly. **This is the one that fails quietly** and deserves care.
- `targets/nordic-zephyr/zephyr/CMakeLists.txt:247` — explicit `${SRC_DIR}/qr/qrcode.c`, repoint to
  `${THIRD_PARTY_DIR}/QRCode/src/qrcode.c` alongside the existing `uzlib` and `bb_epaper` entries.
- `targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:261` — explicit `"../qr/qrcode.c"`,
  repoint. Per CLAUDE.md this file is hand-maintained despite its "do not edit" banner; the
  `.slcp` needs no change because `third_party/` already sits outside the SLC project directory,
  exactly as `shared/` does.

### `shared/sources.cmake` tier

**No tier.** `third_party/` sources are not listed in `shared/sources.cmake` — that file is the
one-list rule for `shared/` only, and the existing precedent is unambiguous:
`third_party/uzlib/src/od_zlib_stream.c` and `third_party/bb_epaper/src/Group5.cpp` are named
per-target in each target's own build file
(`targets/nordic-zephyr/zephyr/CMakeLists.txt:250-251`), not in `sources.cmake`.

For completeness: were this our own code, it would be `OD_SHARED_SOURCES_PURE`
(`shared/sources.cmake:108`) — it needs only the C standard library. It is not our own code.

---

## 4. Memory

### 4.1 What it allocates

Nothing statically or on the heap. **Everything is stack**, and three of the four buffers are VLAs:

| Buffer | Site | Size |
|---|---|---|
| `codewordBytes[407]` | `targets/nordic-zephyr/src/qr/qrcode.c:471` (= `targets/esp32-idf/src/qr/qrcode.c:334`) | **fixed 407 B** |
| `isFunctionGridBytes[bb_getGridSizeBytes(size)]` | `:492` (= esp32 `:348`) | VLA — 211 B at v6, 407 B at v10 |
| `result[data->capacityBytes]` | `:389` (= esp32 `:275`) | VLA — 407 B (inherits `sizeof codewordBytes`) |
| `coeff[blockEccLen]` | `:391` (= esp32 `:277`) | VLA — 16 B at v6, ≤ 26 B |

Cortex-M33 `-Os -fno-inline` (`arm-none-eabi-gcc` 12.2.Rel1, the toolchain this repo pins):
`qrcode_initBytes` 480 B static + dynamic, `performErrorCorrection` 72 B + dynamic, and
`.text` 3,174 B with zero `.data`/`.bss`.

### 4.2 Measured peak stack

Stack-painting measurement on a dedicated thread, host x86-64 `-O1` (an approximation of the ARM
figure, but a directly measured one rather than a summed `.su` bound):

| Version | Peak stack in `qrcode_initText` |
|---|---|
| 1 | 903 B |
| 4 | 999 B |
| **6 — what all three targets use** | **1,063 B** |
| 10 | 1,271 B |

### 4.3 On a 32 KB BG22 this is significant

`targets/efr32bg22-slc/config/sl_memory_manager_region_config.h:45` sets
`#define SL_STACK_SIZE 2752` — reduced from the Silabs default of 4096.

**Drawing the boot screen consumes ~1,063 bytes, about 39% of the entire BG22 main stack, in one
call** — on top of whatever `opendisplay_display.cpp` already has live, plus the 256-byte
`static uint8_t qr_buf[256]` in BSS (`targets/efr32bg22-slc/opendisplay_display.cpp:363`). It is
transient and confined to boot, and the target does build and link today, so this is a headroom
observation rather than a known failure. But it is the largest single stack consumer I found on
that target, and the BG22 has never been flashed with this build (CLAUDE.md § Status).

### 4.4 A 464-byte stack saving, free and output-identical

`codewordBytes[407]` (`targets/nordic-zephyr/src/qr/qrcode.c:471`) is a **local pessimisation**, not
upstream behaviour. Upstream sizes it from the version actually requested:

```c
uint8_t codewordBytes[bb_getBufferSizeBytes(moduleCount)];   /* upstream src/qrcode.c:798 */
```

where `bb_getBufferSizeBytes(bits) = (bits + 7) / 8` (upstream `:193-195`). Our fork dropped that
helper and substituted the constant 407 — which is `bb_getGridSizeBytes(57)`, the version-10
*grid* size, not a codeword count. The largest codeword buffer any supported version needs is
`2768 / 8 = 346` bytes at version 10. So 407 is over-sized at every version, and *grossly* so at
low versions; and because `performErrorCorrection` derives `result[data->capacityBytes]` from
`sizeof codewordBytes` (`:389`), the error is **paid twice**.

Restoring upstream's derived sizing and re-measuring:

| Version | Current | Derived sizing | Saved |
|---|---|---|---|
| 1 | 903 B | 183 B | 720 B |
| 4 | 999 B | 407 B | 592 B |
| **6** | **1,063 B** | **599 B** | **464 B** |
| 10 | 1,271 B | 1,159 B | 112 B |

Output is unchanged — a 43-case equivalence run over versions 1–10 produced identical masks and
identical bitmaps, 0 failures. At the version every target uses this recovers **464 bytes of BG22
stack, ~17% of `SL_STACK_SIZE`, for a one-line change**.

Two caveats. It keeps a VLA, which some embedded style rules ban outright — three VLAs are already
present, so this changes nothing about that posture, but a fixed `[346]`, or a per-target constant
derived from the maximum version that target uses, are both available and would drop the VLA count
rather than hold it. And it is a change to vendored code, so it belongs in the `NOTICE.md` local-patch
record — restoring upstream's own expression, which is the easiest kind of patch to justify.

I have **not** verified this on hardware, and neither the current nor the corrected figure has been
observed on a flashed BG22.

---

## 5. What de-duplication does and does not buy

Honest accounting, since the flash number is the one most likely to be assumed:

- **Flash: zero saved.** Each target compiles one copy today and would compile one copy after. The
  ~3.1 KB of `.text` is per-image either way.
- **Source: ~1,060 lines and ~13 KB removed** (two of three `.c` copies at 529 lines, two of three
  headers).
- **Licence exposure: four files to keep correct becomes one.** This is the real win, and § 2.4
  shows the current state is already wrong.
- **Correctness: nothing fixed, nothing risked.** The copies already agree byte-for-byte (§ 1.3), so
  there is no latent divergence being closed — only future divergence being prevented.
- **A place to put a host test.** There is no test for this encoder anywhere in the repo today
  (nothing under `tests/host/` references `qrcode_`). One vendored copy makes one differential test
  worth writing; three copies make it awkward to place.

---

## Open questions

Ranked, most consequential first.

1. **Restore the MIT notice regardless of the outcome here?** The de-duplication is optional; the
   licence notice is not. If the move to `third_party/` is deferred, the notice should still be
   added to all six files now. Recommend treating this as a separate, immediate fix.

2. **Take the 464-byte BG22 stack saving, and in which form?** Upstream's VLA (smallest diff, keeps
   a VLA), a fixed `[346]` (no VLA, saves less at v6 — worth measuring), or a per-target
   `OD_QR_MAX_VERSION` constant (best on BG22, adds a knob). All three are output-identical by
   construction; only the first is measured above. This interacts with decision 12's precedent for
   per-target caps.

3. **Should the reduced fork be re-derived from upstream, or vendored as-is?** Vendoring our
   existing 529-line fork is honest and zero-risk. Re-vendoring upstream's 876-line original and
   re-applying the reduction as a documented patch would match how `bb_epaper` is handled and make
   the next upstream bump tractable — but it is strictly more work for a library last released as
   `version=0.0.1` and unlikely to move. I lean to vendoring the fork as-is with § 2.2 recorded as
   the patch.

4. **Which copy becomes the vendored one?** Recommend Nordic/Silabs (`6c8dc1…`), because it retains
   an attribution line and is already 2-of-3. Behaviourally the choice is free. Note this inverts
   the `Firmware`-is-authority default, so it should be written down.

5. **Exact upstream commit — unverified.** `library.properties` says `version=0.0.1` and the
   submodule pointer in the OpenEPaperLink tree is
   `.git/modules/firmware/common/modules/QRCode`, but `git ls-tree` in that tree returned nothing,
   so I could not read the pinned SHA. `NOTICE.md`'s Revision field wants a commit hash; obtaining
   it needs either a working checkout of `OpenEPaperLink/Tag_FW_EFR32xG22` or a hash of our copy
   against upstream's tagged history.

6. **Is the ESP32 `file(GLOB)` a hazard worth fixing separately?**
   `targets/esp32-idf/main/CMakeLists.txt:19-23` globs `${OD_SRC_DIR}/qr/*.c`; when the directory
   disappears the glob matches nothing and the build fails at link, not at configure. Tolerable
   here — a link error is loud — but the glob is the only place in the three build systems where
   removing a source is not a visible edit.

7. **Does `tools/check.sh --targets` cover the BG22 boot screen path at all?** The BG22 builds
   headless but has never been flashed, so a stack change on that target has no runtime gate. Worth
   knowing before touching § 4.4, though the change is provably output-identical.

---

## Verification notes

Verified by execution: the byte-identity of all four variants (65-case differential run); the
byte-identity of each import against its sibling repo (`md5sum`); the absence of `.data`/`.bss`
(`nm`); ARM stack usage and code size (`arm-none-eabi-gcc -fstack-usage`, Cortex-M33 `-Os`); peak
stack (stack painting, host `-O1`); and the equivalence of the corrected buffer sizing (43-case run).

Verified by inspection: upstream identity, licence text and table correspondence; all `file:line`
citations; build-system references; `SL_STACK_SIZE`.

**Not verified:** anything on hardware — no BG22 has been flashed with this or any build; the exact
upstream commit (open question 5); and the OpenEPaperLink → OpenDisplay provenance chain, which is
inference from the submodule remote rather than from OpenDisplay's own history.
