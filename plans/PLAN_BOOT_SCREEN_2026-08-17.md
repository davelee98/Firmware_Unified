# Plan: unify the boot screen renderer

**Status:** PROPOSED, 2026-08-17. No source file is modified by this document.

**Source survey:** [DEDUP_1_BOOT_SCREEN_2026-08-17.md](DEDUP_1_BOOT_SCREEN_2026-08-17.md), whose
design work this plan adopts almost entirely. Where this plan differs from the survey it says so
and gives the evidence — there is one such place and it is § 2.3, which **reverses the survey's
answer to its own open question 1**.

**Re-verified against the tree at `bb5d241` before writing.** Every figure below was measured, not
transcribed. Two of the survey's numbers came out differently and are corrected in place.

---

## 1. Outcome

`esp32-idf` and `nordic-zephyr` render the boot screen from one shared source. Four recorded
defects close, two of them user-visible on shipped hardware. The renderer becomes the first
subsystem in this repo with a **golden-output host test**, which is the real prize: it is a pure
function of (geometry, facts) → packed rows, and nothing about it is testable today.

`efr32bg22-slc` takes the *payload* half immediately and the *renderer* half only if a measured
stack number allows. That split is load-bearing and is in the design from day one, not retrofitted.

**Measured scale of the duplication** (`diff -w`, counting changed lines both sides):

| Pair | Differing lines | Of |
|---|---:|---|
| `esp32-idf` ↔ `nordic-zephyr` | **405** | 1054 / 1041 |
| `../Firmware` (authority) ↔ `esp32-idf` snapshot | **122** | 1072 / 1054 |

The survey reported 173 for the second pair; the tree now measures 122. Either upstream moved or
the survey counted differently — **re-measure at implementation time and do not trust either
number**, because § 4's defect B2 is defined as "whatever is in that diff and behavioural".

Duplicated verbatim beyond the renderer itself:

- `logo_bitmap.h` — **byte-identical** on both targets (`bb6250d2…`), **111,510 B each**. Nordic's
  copy is entirely dead (§ 4, B1).
- `qr/qrcode.c` — Nordic and Silabs **byte-identical** (`6c8dc10b…`); ESP32's differs (`d8aecca…`)
  and is the same code reformatted. Three copies, one behaviour.

---

## 2. The three decisions

### 2.1 Two targets, not three — and that is decision 9, not timidity

BG22's boot screen is a **third independent derivation**
(`targets/efr32bg22-slc/opendisplay_display.cpp`, ~210 lines inside a 921-line file), not a
stripped copy. Different font, different layout solver, no rotation, no logo, no zones, no
swatches. Adopting a renderer designed for 1872×1404 three-zone layouts onto a 32 KB part, to
render none of those things, is CLAUDE.md decision 9 ("no lowest-common-denominator features") run
in reverse.

So Phase 1 merges two targets. **BG22 is a separate decision with its own gate** (§ 6, C-B4), and
the two-file split in § 3.2 exists so it can take the cheap half without the expensive one.

### 2.2 The drawing surface is a link-time seam, not a vtable and not a framebuffer

The survey's § 4.1 analysis is adopted unchanged and its reasoning is correct:

- **A caller-supplied framebuffer is arithmetically impossible.** 800×480 mono is 48,000 B; BG22
  has 32 KB total. The row-at-a-time design in all three implementations exists *because* there is
  no framebuffer.
- **`od_hal_panel` cannot express it.** It lives in `targets/esp32-idf/hal/`, only ESP32
  implements it, its `begin()` takes no plane argument, and the FastEPD arm needs a *positional*
  row write (`y`), not a stream.
- **A link-time `extern "C"` seam is the pattern this repo already uses** three times over
  (`od_cmd_app.h`, `od_session_app.h`, `od_rxq_app_report`). Decision 1's stated mechanism;
  decision 2's "one vtable, deliberately" is untouched.

### 2.3 Split-panel emission — the survey's open question 1, answered, and the answer is the opposite

The survey ranked this first because it *shapes the seam*, and asked whether the missing
half-plane path in `targets/esp32-idf/src/boot_screen.cpp` is deliberate omission or import drift.
It guessed drift and said "ask whoever did the import." Reading `../Firmware/src/split_panel.h`
answers it without asking, and the answer changes the seam:

> Dual-controller ("split buffer") e-paper panels — a single glass driven by two controllers on
> separate chip-selects, each owning one half of every row. Currently the Seeed reTerminal
> **E1004's 13.3" 1200x1600 Spectra6 (T133A01)** and the **8.1" 1024x576 Spectra6**.
>
> Selection is entirely at **RUNTIME**: `splitPanelUsed()` tests `BBEP_SPLIT_BUFFER` on
> `bbep.iFlags`, which `bbepSetPanelType()` fills in from the panel table for whatever
> `panel_ic_type` the device config names. **There is no build flag.**

Three consequences, all verified in the tree:

1. **It is not board-gated, so retiring the `s3-e1004` *board* did not retire it.** The panel is
   selected by device *config* at runtime. Any S3 board can be configured for a split panel. The
   tempting inference — "E1004 is retired, so the split path is dead" — is wrong, and it is the
   inference someone will make if this is not written down.
2. **The capability is absent from the entire unified repo.** `targets/esp32-idf/src/split_panel.*`
   does not exist, no target references `splitPanelUsed()`, and `nordic-zephyr` has no copy either.
   Only `../Firmware` has it. The snapshot knows the *scheme*
   (`OD_COLOR_SCHEME_BWGBRY_SPLIT` appears in the palette and label code) but cannot emit to the
   glass. **That is a capability regression against upstream, larger than the boot screen and not
   caused by it.**
3. **The 8.1" 1024×576 Spectra6 is one of the two split panels — and BG22's own panel map already
   lists it** (`opendisplay_epd_map.c:50`, `case 0x002B: return EP81_SPECTRA_1024x576`). Whether
   BG22's wiring of that panel is genuinely dual-controller is **UNVERIFIED** and is a question for
   C-B4, but the identifier is there today.

**Decision: the seam carries half-plane emission from day one; this plan does not implement the
split-panel transport.** Concretely, `od_boot_app_begin_plane()` takes the segment count and
`od_boot_app_write_row()` takes a length that may be half the pitch, so a later split-panel
promotion is a target-side implementation rather than a seam redesign. Designing the seam without
it and adding it later means redesigning it later, which is precisely what the survey warned about.

**File the transport gap separately.** `FOLLOWUPS.md` gains an entry: the unified repo cannot drive
dual-controller panels at all, and a device configured for one gets no correct output from any
path, boot screen or image. That is not this plan's to fix.

---

## 3. Architecture after

### 3.1 The seam

```c
/* shared/core/od_boot_app.h — the boot renderer's target seam.
 * Plane identity is normalised here; targets map to bb_epaper's PLANE_0/PLANE_1. */
#define OD_BOOT_PLANE_PRIMARY  0   /* mono / packed / BWR-BWY B&W / gray4 LSB */
#define OD_BOOT_PLANE_SECOND   1   /* BWR-BWY colour / gray4 MSB */

/* Open a plane. `segments` is 1 for an ordinary panel and 2 for a dual-controller one, where
 * each row arrives as `segments` calls of pitch/segments bytes, left half first. Declaring it
 * here rather than discovering it later is § 2.3. */
int  od_boot_app_begin_plane(int plane, uint16_t w, uint16_t h, uint8_t segments);
/* One packed native row segment. `y` is supplied because FastEPD writes positionally. */
int  od_boot_app_write_row(uint16_t y, uint8_t segment, const uint8_t *row, uint16_t len);
void od_boot_app_end_plane(int plane);

/* Facts the renderer cannot derive. One target function each. */
int   od_boot_app_bits_per_pixel(void);   /* FastEPD's ED103TC2 override */
int   od_boot_app_default_plane(void);    /* getplane() / boot_get_plane() */
bool  od_boot_app_direct_2bpp(void);      /* gray4 without the plane split */
uint8_t od_boot_app_segments(void);       /* 1, or 2 for a dual-controller panel */
void  od_boot_app_device_id_hex(char out[7]);
void  od_boot_app_firmware_version(uint8_t *maj, uint8_t *min, uint8_t *patch);
float od_boot_app_battery_volts(void);    /* < 0 = unknown */
float od_boot_app_chip_temp_c(void);      /* < -900 = unknown */
```

```c
/* shared/core/od_boot_screen.h */
struct od_boot_bufs {
    uint8_t *row;  size_t row_len;   /* >= pitch, or pitch + planePitch when gray4 splits */
    uint8_t *qr;   size_t qr_len;    /* >= 256; static on BG22, stack elsewhere */
};
bool od_boot_screen_render(const struct od_config *cfg,
                           const struct SecurityConfig *sec,
                           const struct od_boot_bufs *bufs);
```

**Every large buffer is caller-supplied and caller-sized, and the renderer REFUSES rather than
truncates.** Row scratch is 960 B on ESP32, 680 B on Nordic, 256 B on BG22 — a shared fixed size
would be sized for the biggest target, which the memory rule forbids. `qr` is caller-supplied
because BG22 already makes it `static` deliberately to keep 256 B off a 2,752-byte stack
(`SL_STACK_SIZE`, verified at `config/sl_memory_manager_region_config.h:45`); a shared renderer
that put it back on the stack would silently undo that.

Refusing on a short buffer promotes ESP32's existing guard and makes Nordic's separate 340 B
`s_gray4_plane_scratch` unnecessary. Same rule as decision 12: **refuse, never truncate.**

### 3.2 Two shared sources, deliberately

| Source | Tier | Why separate |
|---|---|---|
| `shared/core/od_boot_payload.c` | **PURE** | base64url, hex, key-redaction policy, the 23-byte landing payload, the URL. No HAL, no seam. All three targets already agree on these bytes and one of them has an **external consumer** — the opendisplay.org landing service — so this is a wire-ish contract that deserves pinning on its own. |
| `shared/core/od_boot_screen.c` | **APP_BOOT** (new tier) | Needs `od_boot_app.h`. |

A new tier is required and is not gratuitous: `od_boot_screen.c` needs none of `od_hal_adv`,
`od_hal_crypto`, `od_hal_radio` or `od_hal_wdt`, and folding it into an existing APP_* tier would
force a target to take the renderer in order to get the session code. Naming a tier for a **seam**
rather than a HAL already has two precedents (APP_SESSION, APP_RXQ).

Consumers: `esp32-idf` and `nordic-zephyr` take PURE + APP_BOOT. `efr32bg22-slc` takes PURE
unconditionally; APP_BOOT only if C-B4 passes.

### 3.3 The vendored neighbours

- **`qr/qrcode.c` → `third_party/qrcode/`.** Vendored MIT code, exempt from the one rule by
  decision 13. Nordic and Silabs are byte-identical — adopt that as canonical and reformat ESP32's
  onto it. One copy, three consumers, exactly as `third_party/bb_epaper` already works.
- **`logo_bitmap.h` → one home**, with `tools/convert_logo.py` moved beside it. It is a
  **generated asset, not shared logic**; 111 KB under `shared/core/` would misrepresent what
  `shared/` is. Inclusion stays compile-gated (`OD_BOOT_LOGO_ENABLE`), default on where flash
  allows and off on BG22.

---

## 4. The defects this closes

All four are verified in the tree, not inferred.

| # | Defect | Evidence | Closed by |
|---|---|---|---|
| **B1** | **Nordic renders no logo, silently, and has since import.** Its `boot_screen.cpp` has three `#ifdef BOOT_HAS_LOGO` blocks (`:896`, `:929`, `:1012`) and **nothing anywhere defines the macro** — ESP32 defines it at `:15`, inside the `#if __has_include("logo_bitmap.h")` guard that Nordic's copy dropped. So ~100 lines are unconditionally dead and `targets/nordic-zephyr/src/logo_bitmap.h` (111,510 B, byte-identical to ESP32's) is linked by nothing. | verified: `grep BOOT_HAS_LOGO` finds three uses and zero definitions on Nordic | one gate (`OD_BOOT_LOGO_ENABLE`) in shared code, so "defined nowhere" stops being representable |
| **B2** | **Both targets are behind `../Firmware` on a swatch-fill fix** landed upstream 2026-08-10. Survey § 2.4.1. | 122 `diff -w` lines against the authority | porting the authority's form, per the migration rule |
| **B3** | **QR position divergence.** ESP32/upstream use `modulePx * (qrSize + quiet)`; Nordic uses `qrPx` and its comment claims the ESP32 form clips the right edge. One of them is wrong on real glass. | `boot_screen.cpp:311-316` (nordic) vs `:246-250` (esp32) | § 6 C-B2 adjudicates with evidence; **this is the one place the authority rule may point at the wrong answer**, so it needs an explicit recorded override rather than a silent choice |
| **B4** | **BG22's key-display policy differs** in a way the survey calls arguably user-visible (§ 3.4). | survey § 3.4 | `od_boot_payload.c` settles it for all three, in Phase 2 which is cheap |

**B3 is the one that can go wrong quietly.** A clipped QR quiet zone still scans on most readers,
so "it scanned" is not evidence. Adjudicate it with a photograph of a landscape panel under both
forms, or by rendering both to a host golden image and measuring the right-edge margin in modules.

---

## 5. What makes this worth doing: it becomes testable

This is the strongest argument and it is not the line count.

The renderer is a pure function of (geometry, facts) → packed rows. Once the surface is a seam, a
host test implements `od_boot_app_*` over a `malloc`'d frame and pins a **hash per case** across
scheme × rotation × representative panel geometry. Today `tools/check.sh` has **no boot-screen
check of any kind**, and there is no boot-screen test in `tests/host/`.

That matters more than usual because both B1 and B2 are exactly what such a test catches for free:
B2 is a rendering bug that shipped and was found by eye on hardware, and B1 is a missing element
nobody noticed across two repos and an import. A golden-hash test would have failed on the import
commit that dropped Nordic's `__has_include` guard.

---

## 6. Commit sequence

Each commit builds all three targets and finishes `tools/check.sh --targets` with zero failures
and zero skips.

| Commit | Content | Required proof |
|---|---|---|
| **C-B0** | `third_party/qrcode/` — one copy (the Nordic/Silabs byte-identical form), three consumers. No behaviour change. | all three targets link; images **byte-identical** on Nordic and Silabs, since their bytes are already canonical |
| **C-B1** | `shared/core/od_boot_payload.c` (PURE) + host test. Both targets swap onto it. Settles **B4**. | differential: the new payload/URL/redaction output matches each target's current output byte-for-byte on a corpus of device ids, keys and versions — **including the all-zero key** |
| **C-B2** | **Adjudicate B3 and record it.** No renderer code. Render both QR forms to host goldens, measure the right-edge quiet zone in modules, and if Nordic is right write the override into `docs/DIVERGENCE_MATRIX.md` with the evidence. | a written decision with a number or a photograph; **not** "it scanned" |
| **C-B3** | `shared/core/od_boot_app.h` + `od_boot_screen.c` (new APP_BOOT tier) + the golden-hash host test. `esp32-idf` swaps onto it; its `boot_screen.cpp` becomes ~60 lines of seam. Lands **B2**'s upstream fix and C-B2's verdict. | goldens pinned for every scheme × rotation × geometry; ESP32 golden set **matches the pre-swap renderer** except at the pixels B2 and B3 deliberately change, each enumerated |
| **C-B4** | `nordic-zephyr` swaps onto the same source. Closes **B1** — the logo appears for the first time. | Nordic golden set now equals ESP32's for equal geometry; **hardware: the logo is photographed on a flashed board**, because it has never rendered |
| **C-B5** | BG22 takes `od_boot_payload.c` only. Delete the three retired copies and the dead Nordic asset. | BG22 `.text`/`.bss` delta recorded; no renderer change |
| **C-B6** | **Decision commit, may be "no".** BG22 adopts APP_BOOT, or records why not. | `-fstack-usage` on a BG22 prototype **before** committing, against `SL_STACK_SIZE` 2752 and the 480 B of main-RAM headroom C13 recorded. A number, not a principle |

C-B0 through C-B2 are independent of the rest and can land while B3 is still being argued.
C-B3 must precede C-B4. C-B6 may never happen.

---

## 7. Automated verification

- **Golden hashes**, the centrepiece: every colour scheme × rotation × representative panel
  geometry, hashed. Generated from the ESP32 renderer *before* the swap so the first run is a
  regression test rather than a fresh baseline.
- **Deliberate diffs are enumerated, not tolerated.** B2 and B3 change pixels. Each gets its own
  golden pair (before/after) and a one-line statement of what moved. A golden that changes without
  an entry is a failure.
- **Payload differential** (C-B1) against each target's current output, all-zero key included.
- **Short-buffer refusal**: `row_len` one byte under `pitch` returns false and writes nothing;
  same for the gray4 `pitch + planePitch` case. Shown failing with the guard removed.
- **Segment contract**: a `segments = 2` render emits `2 × height` calls of `pitch/2`, left half
  first, even though no target implements the transport yet — this is what stops § 2.3's seam
  decision from silently rotting.
- **`tools/check.sh` gains a boot-screen check.** There is none today.

---

## 8. Hardware gates

| Row | Observation | It distinguishes |
|---|---|---|
| ESP32 boot screen unchanged | photograph before/after C-B3 matches except at B2/B3 pixels | that the extraction is behaviour-preserving |
| **Nordic logo appears** | photographed on a flashed `xiao_nrf52840` | **B1** — it has never rendered, so this row fails by construction before C-B4 |
| Nordic boot screen otherwise unchanged | before/after photograph | that closing B1 did not disturb layout |
| QR scans on both targets | a phone resolves the landing URL | the payload path end-to-end |
| BWR/BWY swatch band | photograph on a colour panel | **B2**, which changes what these boards display |

`xiao_nrf54l15` and `xiao_nrf54lm20a` have never been flashed
([HARDWARE_MATRIX.md](../docs/HARDWARE_MATRIX.md)), so Nordic rows are `xiao_nrf52840` only and
must say so.

---

## 9. Risks and stop conditions

- **Stop if the seam cannot express half-plane emission** without a target-visible change (§ 2.3).
  Redesigning it after C-B3 means redoing C-B3 and C-B4.
- **Stop if BG22 adoption needs the stack grown.** The answer to a 550 B increase against 480 B of
  headroom is not a bigger stack; it is C-B6 answering "no", which is a legitimate outcome the
  two-file split exists to permit.
- **Do not bundle BG22 into Phase 1.** § 2.1.
- **Do not let a golden change land unexplained.** The whole value is that pixel changes become
  visible; a test updated to match new output is the failure mode this replaces.
- **`../Firmware` is the authority except at B3**, and that exception must be written into
  `DIVERGENCE_MATRIX` with evidence, not assumed because Nordic's comment sounds confident.
- **Re-measure the upstream diff before porting.** 122 lines today vs the survey's 173; the set,
  not the count, is what C-B3 must carry.
- **The logo asset must not land under `shared/`.** 111 KB of generated bitmap there misrepresents
  what `shared/` is for.

---

## 10. What this plan does not do

- No BG22 renderer adoption — that is C-B6 and may be declined.
- **No split-panel transport.** The seam accommodates it; nothing implements it. The repo-wide gap
  is filed in `FOLLOWUPS.md` (§ 2.3) and is larger than the boot screen.
- No `od_hal_panel` promotion or change — explicitly out of scope (§ 2.2).
- No change to what the boot screen *says*; only B2 and B3 change what it *looks like*, both
  deliberately and both enumerated.
- No new wire surface. The boot screen has none — the landing URL is its only external contract,
  and C-B1 pins it rather than altering it.
