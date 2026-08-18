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

**The base is `../Firmware/src/boot_screen.cpp`, the live sibling repo — not the `esp32-idf`
snapshot.** Owner decision, 2026-08-17, and it is CLAUDE.md's own rule rather than a new one:
*"THE AUTHORITY IS `../Firmware/`, THE SIBLING REPO — NOT `targets/esp32-idf/src/`. That directory
is a snapshot taken at import, and upstream keeps moving."* Nordic conforms to that base, or its
difference is **ported into** it and justifies itself in writing.

Extracting from the sibling rather than the snapshot has three consequences worth stating, because
each removes work the earlier draft had scheduled:

- the 122-line snapshot drift is **carried in by construction**, so B2 stops being a port and
  becomes a property of the base;
- the split-panel half-plane loop **comes with it** (§ 2.3), so the seam is validated against real
  emitting code rather than designed speculatively;
- `esp32-idf`'s swap becomes a genuine behaviour change to verify, not a no-op refactor — it gains
  everything upstream landed since the import.

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

### 2.1 All three targets, with BG22 taking a compile-time subset

Owner decision, 2026-08-17: **BG22 adopts the common implementation, subset as needed.** An
earlier draft of this plan argued for two targets and called BG22 adoption "decision 9 run in
reverse". That objection was aimed at the wrong thing and is withdrawn — but the distinction it was
reaching for is real and becomes this section's governing rule.

**What BG22 actually has**, counted in `targets/efr32bg22-slc/opendisplay_display.cpp` rather than
assumed:

| | BG22 today | shared renderer |
|---|---|---|
| QR code | yes (56 references) | yes |
| text lines | 5 — domain, name, fw, key ×2 (`:434`) | superset |
| layout/scale solver | yes (37 references) | yes, richer |
| rotation | yes — `bbepSetRotation()` at `:706`, `:752` | yes |
| header zone | **none** (0 references) | yes |
| footer zone | **none** (0) | yes |
| colour swatches | **none** (0) | yes |
| logo | **none** (0) | yes |

So the subset is precise: **BG22 declines zones, footer and swatches. It keeps everything else, and
it gains a logo it never had** (§ 3.3.1, S1 + S2 at 2,084 B). "Subset as needed" is therefore not a
reduction of BG22's boot screen — it loses nothing it renders today and gains one element.

Amusingly the two implementations already agree on the threshold that matters: BG22 picks its text
scale with `(w >= 400u && h >= 300u) ? 2 : 1` (`:441`), the same 400×300 boundary the shared
renderer uses for `useZoneLayout` (`../Firmware:581`). They were derived independently and landed
on the same number.

#### The rule: follow the math, do not hardcode the absence

An earlier revision of this section proposed `OD_BOOT_ZONES_ENABLE=0` and
`OD_BOOT_SWATCH_ENABLE=0` on BG22, reasoning that it renders no zones today. **That was wrong, and
it was the exact failure decision 9 names — proposed in the same section that warned against it.**

The shared renderer already decides zones by geometry, at runtime:

```c
useZoneLayout = (w_log >= 400 && h_log >= 300)     // ../Firmware:581
```

**BG22 drives 13 panel types that satisfy that predicate**, counted from its own map against the
vendored panel table:

| | |
|---|---|
| 400×300 | `EP42_400x300`, `EP42B_`, `EP42R2_`, `EP42YR_` |
| 648×480 | `EP583_648x480` |
| 680×480 | `EP1085_1360x480` |
| 800×480 | `EP426_800x480`, `EP426_..._4GRAY`, `EP75_..._4GRAY`, `EP75_..._4GRAY_GEN2`, `EP75YR_`, `EP397YR_` |
| 1024×576 | `EP81_SPECTRA_1024x576` — dual-CS, refused under B5 |

So BG22 renders no zones today **because its own renderer never implemented them**, not because
its geometry excludes them. Hardcoding the absence would deny zones, footers and swatches to a
dozen panel types the shared math says should have them — freezing an implementation gap into a
compile-time fact and calling it a capability.

> **The rule: a target follows the shared layout math. A compile-time gate is only legitimate
> where the math can never select the thing gated out.**

That distinction is what separates the two gates this plan does keep from the two it just dropped:

| Gate | Legitimate? | Why |
|---|---|---|
| `OD_BOOT_ZONES_ENABLE` | **no — dropped** | the math selects zones on 13 BG22 panels |
| `OD_BOOT_SWATCH_ENABLE` | **no — dropped** | same; swatches live in a footer the math asks for |
| `OD_BOOT_LOGO_SIZES=2` | **yes** | S3 needs `w_log >= 1664`; BG22's largest drivable panel is 800 px wide after B5. The math can never select it |
| caller-supplied `qr` buffer | **yes** | not a feature gate at all — the same bytes, in static instead of stack |

The logo gate survives precisely *because* it follows the math rather than overriding it, and the
same arithmetic independently confirms the § 3.3.1 choice: 800×480 gives `w_log = 800 >= 514` and
`headerH = 96 >= 96`, so **S2 is reachable on BG22 and S1 alone would have been too little.**
S1 + S2 is what the math asks for.

**BG22 therefore gains zones, footers and swatches on its larger panels**, alongside the logo.
"Subset as needed" turns out to subset almost nothing: the only thing BG22 compiles out is an
asset its geometry cannot reach.

#### What that costs, stated honestly

Dropping the zone gate removes the ~210 B of stack it was claimed to save (`manufLine`,
`modelLine`, `footerSchemeStr`, `footerResStr`, `footerSegs`, `footerSegX`, `vStr`, `tStr`). The
budget is now:

| | |
|---|---:|
| survey's full-frame estimate | ~900 B |
| less the QR buffer moved to static | −256 B |
| **estimated shared frame on BG22** | **~644 B** |
| BG22's current boot frame | ~300–350 B |
| `SL_STACK_SIZE` | 2,752 B |

**C-B6's measurement is now genuinely load-bearing rather than a formality**, and the plan should
say so: roughly 640 B of a 2,752 B stack, in a call chain that is not the deepest in the system.
If `-fstack-usage` says it does not fit, the answer is *still* not to hardcode a layout absence —
it is to reduce the frame (compose zone strings into the existing scratch, shorten the footer
segment arrays), or to raise the stack against measured headroom.

Its dual-CS refusal (§ 2.4, B5) is independent of all of this and lands regardless.

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

### 2.3 Split-panel emission — verified against the sibling repo, and it is live current work

The survey ranked this first because it *shapes the seam*, guessed the missing half-plane path in
`targets/esp32-idf/src/boot_screen.cpp` was import drift, and said "ask whoever did the import."
The sibling repo answers it, and the answer is stronger than drift: **dual-controller panel support
is new, deliberate, current upstream work.**

`../Firmware` commit **`20807f3` (2026-08-10, PR #147)** — *"E1004 / dual-controller panels: pin
bb_epaper upstream, replace the fork shim with a runtime-gated bufferless module"* — added
`src/split_panel.{h,cpp}` and its boot-screen integration. Its own commit message settles three
things this plan needs:

1. **Dropping the `esp32-s3-E1004` env was correct and is unrelated to the panel.** Upstream:
   *"it was the same hardware as `esp32-s3-N32R8-extuart`, and the panel is selected at runtime by
   `panel_ic_type`, not at build time."* So the retired **board** and the live **panel** are
   independent facts, and the inference "E1004 is retired, so the split path is dead" is exactly
   backwards. The panel is reached by configuring any S3 board for it.
2. **The module exists because a vanished vendor macro silently disabled the panel.** The old shim
   was guarded by `#ifdef BBEP_T133A01` — a *bb_epaper fork* define, not a firmware one. Repinning
   to upstream bb_epaper removed it, so *"all 12 guarded regions compiled to nothing while
   `pio run -e esp32-s3-E1004` still reported SUCCESS: firmware that could not drive the panel,
   with no error and no log line."* The replacement is gated at **runtime** on
   `bbep.iFlags & BBEP_SPLIT_BUFFER`, with `static_assert`s so a future rename is a compile error.
3. **Bufferless by necessity.** bb_epaper's own dual-CS writer needs a 960 KB `ucScreen`, so the
   data phase is driven from the library's CS primitives directly.

**The capability gap in this repo is live, not theoretical, and I verified both halves:**

- The vendored library **already flags both split panels**:
  `third_party/bb_epaper/src/bb_ep.inl:4122` (`EP81_SPECTRA_1024x576`) and `:4157`
  (`EP133_SPECTRA_1200x1600`) both carry `BBEP_SPLIT_BUFFER`, defined at
  `third_party/bb_epaper/src/bb_epaper.h:319`, and the library branches on it in three places.
  So a unified-repo device configured for either panel **does** get the flag set.
- **No target has the module.** `targets/*/split_panel.*` does not exist and nothing calls
  `splitPanelUsed()`. The application half is simply absent.

Together those reproduce the exact condition PR #147 was written to fix — the flag is set, the
build succeeds, and no correct data reaches the glass. **That is a live defect in this repo,
larger than the boot screen and not caused by it.** `FOLLOWUPS.md` gains an entry naming
`../Firmware` `20807f3` and `src/split_panel.{h,cpp}` as the port target; BG22's panel map already
lists `EP81_SPECTRA_1024x576` (`opendisplay_epd_map.c:50`), so it is not an ESP32-only question.

**It is also the same defect shape as B1.** A capability that disappears because a macro stopped
being defined, while every build still passes, is what both PR #147 and § 4's B1 are. That is the
argument for the rule in § 3.1: a capability must never be expressible as "defined nowhere."

### 2.4 Dual CS: Nordic gains it, Silabs declines it — and declining must be loud

Owner decisions, 2026-08-17:

- **`nordic-zephyr` supports dual CS.** The `split_panel` port is therefore not an ESP32 errand;
  both targets take it, and the boot renderer's half-plane path has two real consumers rather than
  one. This settles § 2.3's seam question in the strongest direction: the segment contract is
  exercised, not merely reserved.
- **`efr32bg22-slc` does not support dual CS.** That is a capability declaration, and this repo
  already has the mechanism for exactly that — `shared/core/od_caps.h`, added by C13 so a target
  can state an absence that `shared/` cannot infer. Dual CS becomes its second user:
  `OD_CAP_DUAL_CS`, default `1`, with BG22 defining `0`.

**Declining is not the same as ignoring, and B5 is what ignoring looks like today.** BG22 already
accepts `panel_ic_type = 0x2B` and maps it to `EP81_SPECTRA_1024x576`
(`opendisplay_epd_map.c:50`), a panel the vendored table flags `BBEP_SPLIT_BUFFER | BBEP_7COLOR`
(`bb_ep.inl:4122`). `bbepSetPanelType()` sets the flag, the library branches on it in three
places, and BG22 has no CS2 pin, no CS2 config field and no dual-CS code path. The panel is
half-driven and nothing says so.

**The refusal is a runtime test on the flag, not a compile-time check on the map.** After
`bbepSetPanelType()` succeeds, BG22 tests `iFlags & BBEP_SPLIT_BUFFER` and fails the panel
configuration loudly when `OD_CAP_DUAL_CS == 0`. Two reasons it must be that way round:

1. **A macro can vanish; a flag cannot.** PR #147 exists precisely because 12 regions guarded by
   `#ifdef BBEP_T133A01` compiled to nothing when the define disappeared, while the build still
   passed. Gating on the runtime flag is the idiom upstream chose after being burned, and
   `static_assert`s on the flag value make a rename a compile error.
2. **The map is not the authority on which panels are dual-controller — the library table is.**
   Deleting `0x2B` from BG22's map would fix today's instance and miss the next panel the vendor
   flags. Testing the flag covers both.

This is the same rule as decision 12's config cap: **refuse, never truncate.** A tag that will not
drive a panel should say so at configuration time, not render half a frame.

#### What this means for the seam

Upstream's emission structure is now known exactly, and it is **not** a per-row segment tag:

- The half-plane loop is the **outer** loop (`splitHalfPasses`), with plane passes inside it
  (`boot_screen.cpp:933`).
- **Only half-pass 0 opens the frame** (`splitPanelBeginFrame()`), and the close is deferred past
  both halves — *"the chip selects stay held until `splitPanelCloseFrame()`"* (`:1052`).
- The sink is a **byte stream that tracks its own crossover**: `splitPanelSinkBytes(data, len)`
  counts against `halfPlaneBytes()` and calls `advanceToRightHalf()` itself, and it **faults
  loudly on excess bytes** rather than dropping them, which the shim it replaced did silently
  (`split_panel.cpp`).
- Each row therefore arrives as `pitch/2` bytes, left half for all rows first, then right.

**Decision: the seam carries half-plane emission from day one, shaped as an outer pass with a
deferred close.** `od_boot_app_begin_plane()` takes the segment count so a target can open once
and close after the last segment; `od_boot_app_write_row()` carries both `y` and the segment index
because FastEPD writes **positionally** while the split sink is a pure **stream** — the two
contracts are different and only the target can reconcile them. This plan does **not** port the
transport; it makes the later port a target-side implementation rather than a seam redesign.

---

## 3. Architecture after

### 3.1 The seam

```c
/* shared/core/od_boot_app.h — the boot renderer's target seam.
 * Plane identity is normalised here; targets map to bb_epaper's PLANE_0/PLANE_1. */
#define OD_BOOT_PLANE_PRIMARY  0   /* mono / packed / BWR-BWY B&W / gray4 LSB */
#define OD_BOOT_PLANE_SECOND   1   /* BWR-BWY colour / gray4 MSB */

/* Open a plane. `segments` is 1 for an ordinary panel and 2 for a dual-controller one. The
 * renderer emits ALL rows of segment 0 before any of segment 1 (upstream's outer half-pass), and
 * the target closes only after the last segment -- the split sink holds both chip selects across
 * the pair. Declaring this here rather than discovering it later is § 2.3. */
int  od_boot_app_begin_plane(int plane, uint16_t w, uint16_t h, uint8_t segments);
/* One packed native row segment, pitch/segments bytes. Both `y` and `segment` are supplied
 * because the two consumers disagree: FastEPD writes POSITIONALLY, while the split sink is a
 * STREAM that tracks its own left/right crossover and faults on excess bytes. */
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

#### 3.1.1 BG22 keeps its 256 B row buffer — sized on a fleet fact, not a code limit

Owner decision, 2026-08-17, and the *reason* matters more than the number because it decides what
invalidates it.

**All colour schemes exist on BG22. No shipped BG22 is configured for 4 bpp.** Those are different
statements and only the second sizes the buffer. `opendisplay_display_color.c:17` is correct, not
over-general: BG22's image path genuinely handles `BWGBRY` and `GRAY16` at 4 bpp and `GRAY4` at
2 bpp. What is true is narrower — no device in the field is configured that way.

Against the renderer's pitch formula (`../Firmware:593-597`) at BG22's 800 px maximum, the widest
it can drive once B5 refuses the 1024×576 dual-controller panel:

| Scheme | bpp | pitch at 800 px | shipped on BG22? |
|---|---|---:|---|
| mono / BWR / BWY | 1 | 100 B | yes |
| BWRY / 4-colour | 2 | **200 B** | yes |
| GRAY4 (+ plane split) | 2 | 300 B | **no** |
| BWGBRY / GRAY16 | 4 | 400 B | **no** |

**`max(need)` over the shipped set is 200 B against 256 B — 56 B spare, no change required.**

#### What this makes load-bearing

Because the cap rests on a deployment fact rather than a capability limit, **the runtime refusal
stops being a hypothetical safety net and becomes the thing that actually fires** if the fleet
assumption ever breaks. Two consequences the plan must carry:

1. **The boot renderer's ceiling is narrower than the image path's, on purpose, and that asymmetry
   is now documented rather than accidental.** A BG22 configured for `BWGBRY` would upload and
   display images correctly while its boot screen refused — because the image path is sized for
   4 bpp and the boot row buffer is not. That is a coherent state, not a bug, but it is a
   surprising one to debug from the outside.
2. **The refusal must say which number to change.** `od_boot_screen_render()` returning `false`
   should log the required and available lengths, so the diagnosis is "row buffer 256 < 400
   required" rather than a blank panel. The fix is then one constant: 256 → 400 (+144 B static,
   against the 480 B of main-RAM headroom C13 recorded).

The § 7 golden tests pin the shipped set. They should additionally assert that the **unshipped**
4 bpp and GRAY4 geometries are *refused* at 256 B rather than silently mis-rendered — that is the
one behaviour this sizing decision depends on, so it is the one that needs a test.

#### One thing not to re-derive from

The survey's sizing argument reached the right answer by the wrong route: *"the largest panel is
`EP81_SPECTRA_1024x576`, and 1024 px at 4-colour 2 bpp is exactly 256 B."* A 6-colour Spectra
configures as `BWGBRY`, which is **4 bpp**, so that panel needs 512 B. The figure landed on the
buffer size by applying the wrong bits-per-pixel to the wrong panel. Do not use that sentence to
justify the cap; use the shipped-scheme table above.

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

- **`qr/qrcode.c` → `third_party/qrcode/`** — see § 3.3.2, which is larger than it looks.
- **`logo_bitmap.h` → one home**, with `tools/convert_logo.py` moved beside it. It is a
  **generated asset, not shared logic**; the 111 KB of hex source under `shared/core/` would
  misrepresent what `shared/` is. Inclusion is gated **per size**, not on/off — see below.

#### 3.3.2 The QR encoder: one copy, and a differential before discarding two

Three copies today — `targets/esp32-idf/src/qr/`, `targets/nordic-zephyr/src/qr/`,
`targets/efr32bg22-slc/qr/` — of vendored MIT code, exempt from the one rule by decision 13. They
collapse to `third_party/qrcode/`, exactly as `third_party/bb_epaper` already works.

**The boot screen is its only consumer, which is why this belongs to this plan.** Every file that
includes it, in this repo and in all three sibling repos, is a boot screen:

```
targets/{esp32-idf,nordic-zephyr}/src/boot_screen.cpp
targets/efr32bg22-slc/opendisplay_display.cpp
../Firmware/src/boot_screen.cpp   ../Firmware_NRF54/src/boot_screen.cpp
../Firmware_Silabs/opendisplay_display.cpp
```

Nothing else in the fleet draws a QR code. A standalone plan for a change with one downstream
consumer, written by the effort about to rewrite that consumer, would be ceremony.

**But it is not the file move it was first written up as.** Nordic and Silabs are byte-identical
(`6c8dc10b…`). ESP32's differs (`d8aecca…`) by **312 whitespace-insensitive lines in the `.c` and
21 in the `.h`.** Spot checks look like reformatting and restored comments — table layout,
`#include <limits.h>` moved — consistent with "the same code reformatted", but 312 lines is far
past what anyone should eyeball and then declare cosmetic.

So **C-B0 owes a differential, not an assertion**: encode a corpus of payloads — including the
boot screen's own 23-byte base64url landing payload at every version and ECC level it can select —
through both copies and compare the resulting module bitmaps. Byte-equal output is what licenses
discarding ESP32's copy. If they are *not* equivalent, that is a finding which changes what C-B3
inherits, and it must surface before the renderer extraction rather than during it.

**It lands as its own PR, sequenced first.** Not because it is separable in purpose — it is not —
but because the review question is unrelated to everything after it:

- **zero behaviour change** if the differential passes, so the PR reduces to "are these the right
  bytes"; a renderer extraction cannot be reviewed that way;
- **different governance** — `third_party/` under decision 13, not `shared/` under the one rule;
- **it stands alone.** Three copies to one is worth having even if the renderer work stalls.

#### 3.3.1 The logo gate selects SIZES, and BG22 takes S1 + S2

An earlier draft made this a boolean (`OD_BOOT_LOGO_ENABLE`, "off on BG22"). Measuring it showed
that is the wrong shape, because the three sizes are not remotely equal in cost.

**Measured** — compiled for `cortex-m33` at `-Os` with the BG22 build's own
`-ffunction-sections -fdata-sections`, then linked under `--gc-sections`:

| Linked set | Asset bytes | Share of BG22's ~27.8 KB flash headroom |
|---|---:|---:|
| S1 only (84x44) | 484 | 1.7 % |
| **S1 + S2 (adds 154x80)** | **2,084** | **7.5 %** |
| all three (adds S3, 499x260) | 18,464 | 66 % |

**RAM cost is zero in every configuration.** The arrays are `static const`, so they land in
`.rodata`; the measurement reports `.data 0` and `.bss 0`. The renderer indexes the array directly
per pixel (`bootLogoPixelBlack()` takes `const uint8_t*`), so there is no scratch buffer either.
An unreferenced size is not even emitted — GCC drops it at compile time, before `--gc-sections`
is asked.

**Decision, 2026-08-17: BG22 links S1 + S2. `OD_BOOT_LOGO_SIZES` defaults to 3; BG22 defines 2.**

That is **behaviour-preserving on BG22, not a trade-off**, and the reachability arithmetic is why:

| Size | reachable when `w_log` >= | and `headerH` >= | i.e. landscape `h_log` >= |
|---|---:|---:|---:|
| S2 | 514 | 96 | 480 |
| S3 | 1664 | 276 | 1380 |

BG22's largest map entry is `EP81_SPECTRA_1024x576` — far below S3's 1664 px threshold, and it is
the dual-controller panel B5 makes it refuse anyway. **S3 can never be selected on this part**, so
excluding it removes 16,380 B that could not have been drawn.

Two things this must carry to be honest:

1. **Dropping a size downgrades, it does not disable.** On a target that *can* reach S3 — roughly
   1664x1380 and larger, such as an 1872x1404 panel — excluding it renders S2 instead. Smaller
   logo, still a logo. That is why the gate is not `OD_BOOT_LOGO_ENABLE`.
2. **The selector must not reference an excluded size.** The upstream chain tests S3 then S2
   unconditionally (`../Firmware:864-865`); each branch becomes `#if`-gated on
   `OD_BOOT_LOGO_SIZES`, with S1 remaining the ungated fallback. A host test asserts, for a
   target's declared maximum panel geometry, that every excluded size is unreachable — so setting
   the gate too low is a test failure rather than a silently smaller logo.

**This materially de-risks C-B6.** The survey estimated 4-7 KB of `.text` for the shared renderer
on BG22. At 18,464 B the logo alone would have consumed 66 % of headroom and made renderer
adoption implausible on flash grounds before the stack question was even reached; at 2,084 B the
combined cost is roughly 6-9 KB against 27.8 KB, and C-B6 turns back into the stack decision it
was supposed to be.

---

## 4. The defects this closes

All four are verified in the tree, not inferred.

| # | Defect | Evidence | Closed by |
|---|---|---|---|
| **B1** | **Nordic renders no logo, silently, and has since import.** Its `boot_screen.cpp` has three `#ifdef BOOT_HAS_LOGO` blocks (`:896`, `:929`, `:1012`) and **nothing anywhere defines the macro** — ESP32 defines it at `:15`, inside the `#if __has_include("logo_bitmap.h")` guard that Nordic's copy dropped. So ~100 lines are unconditionally dead and `targets/nordic-zephyr/src/logo_bitmap.h` (111,510 B, byte-identical to ESP32's) is linked by nothing. | verified: `grep BOOT_HAS_LOGO` finds three uses and zero definitions on Nordic | one gate (`OD_BOOT_LOGO_ENABLE`) in shared code, so "defined nowhere" stops being representable |
| **B2** | **Both targets are behind `../Firmware`** — a swatch-fill fix landed upstream 2026-08-10, plus whatever else is in the drift. | 122 `diff -w` lines against the authority | **adopting the authority as the base closes this by construction.** It still changes what BWR/BWY boards display, so it is verified, not assumed |
| **B3** | **The authority's QR placement clips its own right quiet zone** whenever `modulePx * quiet > pad`. Nordic already fixed it. An **upstream defect**, not a Nordic divergence. | `../Firmware:820,883` vs `nordic:925` | **DECIDED 2026-08-17: adopt Nordic's form.** It is ported into the shared base, fixing ESP32 too, and reported back to `../Firmware` |
| **B4** | **BG22's key-display policy differs** in a way the survey calls arguably user-visible (§ 3.4). | survey § 3.4 | `od_boot_payload.c` settles it for all three, in Phase 2 which is cheap |
| **B5** | **BG22 accepts a dual-controller panel it cannot drive.** Its map returns `EP81_SPECTRA_1024x576` for `panel_ic_type` `0x2B`; the vendored table flags that panel `BBEP_SPLIT_BUFFER`; BG22 has **no CS2 concept at all**. `bbepSetPanelType()` sets the flag and the library branches on it, so the panel is half-driven with no error. | map `opendisplay_epd_map.c:50`; flag `bb_ep.inl:4122`; no `cs_pin_2` anywhere under `targets/efr32bg22-slc/` | `OD_CAP_DUAL_CS=0` plus a **runtime refusal** at panel selection (§ 2.4) |

### Not a defect, but fix it in passing: `logoBmp` is uninitialised

`../Firmware:856` declares `const uint8_t* logoBmp;` with no initialiser, while `logoW`/`logoH`/
`logoStride` beside it are zeroed. The whole selection block is inside `if (useZoneLayout)`
(`:860`), which is false for any panel under 400x300 — so on those panels `logoBmp` is never
assigned and is then passed to `bootLogoPixelBlack()` at `:1017`.

**It is safe today, and only by accident of ordering.** Two independent things save it: the call
site is prefixed `useZoneLayout &&`, and `bootLogoPixelBlack()` performs its bounds checks before
touching `bmp`, so `bmpW == 0` returns false without dereferencing. Neither of those is the
declaration, and neither is local to it. Drop the prefix at one call site, or reorder the checks
inside the helper, and it becomes a use of an uninitialised pointer with nothing to catch it.

**C-B3 carries `logoBmp = NULL` at the declaration and a `logoW > 0` guard at the draw site**, so
the invariant is stated once where it can be seen rather than depending on two distant
coincidences. Zero cost, and it removes a trap from code three targets are about to share.

Worth stating why this is not listed with B1-B5: nothing misbehaves today, so it is hygiene
carried by the extraction, not a defect the extraction closes. It is recorded because the
extraction is the moment it is cheap and the last moment it is obvious.

### B3 is decidable by arithmetic, and Nordic is the one contributing behaviour

"Nordic conforms or is ported in" resolves cleanly here, and the direction is *in*. The two forms
differ by exactly one line and the geometry is fully determined:

```
contentRightX = w_log - pad                      (../Firmware:820)
qrPx          = modulePx * (qrSize + 2*quiet)    (:783, quiet = 4)

authority:  qrX = contentRightX - modulePx * (qrSize + quiet)     (:883)
            -> full-box right edge = w_log - pad + modulePx*quiet
nordic:     qrX = contentRightX - qrPx                            (nordic:925)
            -> full-box right edge = w_log - pad, exactly
```

**The authority's form overhangs the content edge by `modulePx * quiet` and therefore falls off
the panel whenever `modulePx * quiet > pad`.** With `quiet = 4` that is `4 * modulePx > pad`,
which is reachable on ordinary geometries. Nordic's form can never clip. Its comment — *"Using
qrSize+quiet here used to shift the code right by one quiet zone and clip the right edge"* —
describes exactly this.

**Decision, 2026-08-17: adopt Nordic's quiet zone.** This is the "or be ported in" half of the
base rule, exercised once and in the direction the arithmetic supports.

Two things follow:

1. **No photograph is needed.** The predicate is arithmetic, so § 7's golden test evaluates
   `qrX + qrPx > w_log` across every supported geometry and QR version and reports the set where
   the authority clips. That is a far better gate than the eye-check the earlier draft proposed —
   a clipped *quiet zone* still scans on most readers, which is precisely why this survived.
2. **It is an upstream bug, so it goes back.** Fixing it only in `shared/` would leave
   `../Firmware` — the authority, and a shipping repo — still clipping. C-B2 files it there.

**Nordic contributes exactly one line of behaviour to the merged renderer.** Every other
Nordic-only line in the 417-line `diff -w` against the authority is Zephyr plumbing, accessors and
scheme constants — the material that *becomes* the `od_boot_app_*` implementation — plus B1, which
is a defect rather than a divergence. That is a checkable claim and C-B4 should re-check it.

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
| **C-B0** | `third_party/qrcode/` — one copy (the Nordic/Silabs byte-identical form), three consumers (§ 3.3.2). **Own PR, sequenced first.** | **A differential proving ESP32's 312-line-divergent copy is behaviourally identical** across a payload corpus at every version/ECC the boot screen can select — that is what licenses discarding it. Plus: all three targets link, and images **byte-identical** on Nordic and Silabs, whose bytes are already canonical |
| **C-B1** | `shared/core/od_boot_payload.c` (PURE) + host test. Both targets swap onto it. Settles **B4**. | differential: the new payload/URL/redaction output matches each target's current output byte-for-byte on a corpus of device ids, keys and versions — **including the all-zero key** |
| **C-B2** | **Adopt Nordic's QR quiet zone into the base (B3)** and report the defect to `../Firmware`. No seam yet. | the golden harness reports the geometry set where the authority's form satisfies `qrX + qrPx > w_log`; that set is non-empty, is recorded, and is empty after the change |
| **C-B3** | `shared/core/od_boot_app.h` + `od_boot_screen.c` (new APP_BOOT tier) + the golden-hash host test. `esp32-idf` swaps onto it; its `boot_screen.cpp` becomes ~60 lines of seam. Lands **B2**'s upstream fix and C-B2's verdict, plus the `logoBmp = NULL` hygiene fix above. | goldens pinned for every scheme × rotation × geometry; ESP32 golden set **matches the pre-swap renderer** except at the pixels B2 and B3 deliberately change, each enumerated |
| **C-B4** | `nordic-zephyr` swaps onto the same source. Closes **B1** — the logo appears for the first time. | Nordic golden set now equals ESP32's for equal geometry; **hardware: the logo is photographed on a flashed board**, because it has never rendered |
| **C-B5** | BG22 takes `od_boot_payload.c` only. Delete the three retired copies and the dead Nordic asset. **Independently: `OD_CAP_DUAL_CS=0` and the runtime refusal (B5, § 2.4).** | BG22 `.text`/`.bss` delta recorded; a config naming `0x2B` is **refused with a log** and the pre-fix tree is shown half-driving it |
| **C-B6** | BG22 adopts APP_BOOT. Only two gates, both math-following: `OD_BOOT_LOGO_SIZES=2` and the static QR buffer (§ 2.1). Zones, footers and swatches follow `useZoneLayout` exactly as on the other two targets, so BG22 **gains** them on its 400×300-and-larger panels. Retires its own renderer. | `-fstack-usage` on the gated build against `SL_STACK_SIZE` 2752, **taken before the swap lands**, plus `.text`/`.bss` deltas. A shortfall is answered with another justified gate, not abandonment. Hardware: the boot screen renders on a flashed BG22 — including the logo it has never had |

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
- **No split-panel transport.** The seam accommodates it and now has two committed consumers
  (§ 2.4), but nothing here implements it. The port target is named — `../Firmware` `20807f3`,
  `src/split_panel.{h,cpp}` — and it is owed to **both** `esp32-idf` and `nordic-zephyr`. The gap
  is live today because the vendored library already flags both panels while no target carries the
  module. Larger than the boot screen, and filed in `FOLLOWUPS.md` rather than absorbed here.
- **No dual-CS support on BG22, ever** — that is now a declared capability (`OD_CAP_DUAL_CS=0`),
  not an unfinished port. What this plan does add there is the **refusal**, so the absence stops
  being silent (B5).
- No `od_hal_panel` promotion or change — explicitly out of scope (§ 2.2).
- No change to what the boot screen *says*; only B2 and B3 change what it *looks like*, both
  deliberately and both enumerated.
- No new wire surface. The boot screen has none — the landing URL is its only external contract,
  and C-B1 pins it rather than altering it.
