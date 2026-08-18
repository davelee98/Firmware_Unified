# DEDUP 1 — the boot screen renderer

Analysis only, 2026-08-17. No source file was modified. Every claim below cites a file and line
I opened; anything I did not verify is marked **UNVERIFIED**.

---

## 0. Executive answer

**Yes for `esp32-idf` ↔ `nordic-zephyr`, and it is the cleanest de-dup candidate left in the
repo.** The two files are the *same program*. Of the 405 whitespace-insensitive differing lines,
roughly 85 % are comments, identifier renames and accessor plumbing; the material behavioural
differences number about a dozen, and **every one of them is one-way drift where one copy is
simply behind the other**. The renderer has no wire surface, no session state, no heap, and is a
pure function of (geometry, facts) → a stream of packed rows — which makes it host-testable to a
golden hash in a way almost nothing else promoted so far has been.

**Qualified for `efr32bg22-slc`.** BG22 *does* have a boot screen — a **third, independent,
much smaller implementation** embedded in
[targets/efr32bg22-slc/opendisplay_display.cpp:350-560](../targets/efr32bg22-slc/opendisplay_display.cpp).
It is not a stripped copy of the other two; it is a separate derivation with a different font, a
different layout solver, no rotation, no logo, no zones, no swatches, and a different key-display
policy that is arguably a **user-visible defect** (§ 3.4). Adopting the full shared renderer there
costs ~600–800 B of a 2,752-byte stack (§ 5.3) for features the part cannot use. The BG22
adoption is a **separate decision from the ESP32/Nordic merge** and must not be bundled with it.

**One defect found while reading, unrelated to any future merge:** Nordic's boot screen renders
**no logo at all**, silently, and has since import (§ 2.3.1). And **both** unified copies are
behind `../Firmware` on a swatch-fill fix landed upstream 2026-08-10 (§ 2.4.1).

---

## 1. Inventory (verified and extended)

| File | Lines | Notes |
|---|---:|---|
| `targets/esp32-idf/src/boot_screen.cpp` | 1054 | `writeBootScreenWithQr()` |
| `targets/esp32-idf/src/boot_screen.h` | 6 | |
| `targets/esp32-idf/src/logo_bitmap.h` | 1176 | 111,510 B source; **compiled** |
| `targets/nordic-zephyr/src/boot_screen.cpp` | 1041 | `writeBootScreenWithQr(BBEPDISP&)` |
| `targets/nordic-zephyr/src/boot_screen.h` | 9 | |
| `targets/nordic-zephyr/src/logo_bitmap.h` | 1176 | 111,510 B source; **never included — dead** |
| `targets/efr32bg22-slc/opendisplay_display.cpp:350-560` | ~210 | `render_boot_screen()`, third implementation |
| `targets/esp32-idf/src/qr/{qrcode.c,qrcode.h}` | 377 + 29 | vendored MIT QR encoder |
| `targets/nordic-zephyr/src/qr/{qrcode.c,qrcode.h}` | 529 + 42 | same encoder, reformatted |
| `targets/efr32bg22-slc/qr/{qrcode.c,qrcode.h}` | 529 + 42 | **byte-identical to Nordic's** (md5 `6c8dc10b…` / `637604562f…`) |
| `targets/esp32-idf/tools/convert_logo.py` | — | the asset generator; exists once |

Sibling authority repos:

| File | Lines | vs. the unified snapshot |
|---|---:|---|
| `../Firmware/src/boot_screen.cpp` | 1072 | 173 diff lines vs `esp32-idf` (`diff -w`) |
| `../Firmware_NRF54/src/boot_screen.cpp` | 1041 | 83 diff lines vs `nordic-zephyr` (`diff -w`) |

`logo_bitmap.h` is **byte-identical in all four repos** — md5 `bb6250d2a8dd9912fb1845ed8a41d53d`
across `Firmware_Unified/targets/esp32-idf/src/`, `Firmware_Unified/targets/nordic-zephyr/src/`,
`../Firmware/src/` and `../Firmware_NRF54/src/`.

The three `qrcode.c` copies are **functionally identical**. I token-normalised each (comments and
whitespace stripped) and diffed: the only surviving differences are brace placement, one
`struct BitBucket codewords;` vs `BitBucket codewords;`
([nordic qrcode.c:470](../targets/nordic-zephyr/src/qr/qrcode.c) vs
[esp32 qrcode.c:333](../targets/esp32-idf/src/qr/qrcode.c)), and a named temporary in
`qrcode_getDataCapacityBytes()`. No algorithmic divergence.

---

## 2. Categorised diff: `esp32-idf` ↔ `nordic-zephyr`

Legend: **(a) drift** — same intent, one copy older/buggier. **(b) adaptation** — required by the
target. **(c) capability** — one target renders something the other cannot.

Every function from `bootGlyph()` through `bootLayoutFit()` — the entire layout solver, the
font, the base64url encoder, the hex formatter, the text/QR/logo pixel predicates, the pixel
packers, the swatch geometry, the header scale search — is **character-identical** between the
two files modulo the renames below. That is roughly 450 lines each.

### 2.1 Drift (a) — the material ones

**2.1.1 The QR is positioned differently, and Nordic says ESP32's version is a bug.**

- esp32: `const int qrDataRightPx = modulePx * ((int)qrSize + (int)quiet); qrX = contentRightX - qrDataRightPx;`
  — [boot_screen.cpp:901-902](../targets/esp32-idf/src/boot_screen.cpp)
- nordic: `qrX = contentRightX - qrPx;` with the comment *"Keep full QR box (data + quiet on both
  sides). Using qrSize+quiet here used to shift the code right by one quiet zone and clip the
  right edge."* — [boot_screen.cpp:923-925](../targets/nordic-zephyr/src/boot_screen.cpp)

Nordic carries the fix; ESP32 and `../Firmware/src/boot_screen.cpp:901` do not. This is the one
divergence where the **non-authority repo is right**, so CLAUDE.md's "Firmware is the authority"
default has to be overridden explicitly rather than by omission.

**2.1.2 The 4-gray LUT-v2 panel list is stale on Nordic.**

- esp32 recognises **two** panels: `return panelIc == 0x0028 || panelIc == 0x0048;`
  ([:553-557](../targets/esp32-idf/src/boot_screen.cpp), comment naming
  `EP426_800x480_4GRAY` and `EP368_792x528_4GRAY`, mirroring py-opendisplay's
  `_GRAY4_CODES_BY_PANEL`)
- nordic recognises **one**: `return panelIc == 0x0028;`
  ([:602-604](../targets/nordic-zephyr/src/boot_screen.cpp))

An EP368 attached to a Nordic board gets the wrong gray codes in the footer swatches. Pure drift.

**2.1.3 The colour-scheme enum is re-declared locally on Nordic, with a retired name.**

Nordic `#define`s its own `COLOR_SCHEME_*` block at
[:20-27](../targets/nordic-zephyr/src/boot_screen.cpp), including `COLOR_SCHEME_GRAY8 7u` — the
name the canonical header retired in favour of `OD_COLOR_SCHEME_SEVEN_COLOR`. ESP32 uses the
canonical `OD_COLOR_SCHEME_*` throughout ([:312-330](../targets/esp32-idf/src/boot_screen.cpp),
[:527-542](../targets/esp32-idf/src/boot_screen.cpp),
[:618-652](../targets/esp32-idf/src/boot_screen.cpp)). Values agree; the names do not. Drift, and
the private `#define` block is exactly the shape that lets a future protocol renumber pass
unnoticed on one target.

**2.1.4 Chip-id derivation round-trips through a string on ESP32.**

- esp32: `getChipIdHex(last6, sizeof(last6))` then three `strtoul(pair, nullptr, 16)` calls to get
  the 3 payload bytes back — [:663-677](../targets/esp32-idf/src/boot_screen.cpp)
- nordic: `opendisplay_ble_chip_id_last24()` → a `uint32_t` used both for `%06lX` and for the
  three payload bytes directly — [:698-708](../targets/nordic-zephyr/src/boot_screen.cpp), with
  the accessor at [:42-48](../targets/nordic-zephyr/src/boot_screen.cpp) documenting that the
  screen must name *the advertised identity*, not a second derivation of it.

Nordic's is strictly better and states the invariant. Drift; also partly (b), since the two
targets get the id from different places.

**2.1.5 `bootFormatKeyLine`/`formatBootKeyDisplay` null-guard.**

Nordic added `boot_sec() == NULL ||` to both
([:219](../targets/nordic-zephyr/src/boot_screen.cpp),
[:232](../targets/nordic-zephyr/src/boot_screen.cpp)); ESP32 reads a global and cannot be null
([:153](../targets/esp32-idf/src/boot_screen.cpp),
[:166](../targets/esp32-idf/src/boot_screen.cpp)). Adaptation *except* that Nordic then
dereferences `boot_sec()->encryption_key` unguarded at
[:763-764](../targets/nordic-zephyr/src/boot_screen.cpp) — the guard is inconsistent with its own
call site. Latent null deref on Nordic when `useZoneLayout` is true and security never parsed.

**2.1.6 Nordic logs the landing URL; ESP32 does not.**

`od_log_info("boot QR id=OD%s url=%s", last6, url);` —
[:723](../targets/nordic-zephyr/src/boot_screen.cpp). When
`OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN` is set, that URL **contains the 16-byte encryption key** in
base64url ([:709-713](../targets/nordic-zephyr/src/boot_screen.cpp)). The key is on the panel
anyway in that mode, so it is not a new disclosure, but it is a policy divergence that should be
decided once rather than per target.

**2.1.7 `-Werror=maybe-uninitialized` initialisers.**

esp32 initialises `modulePx/qrPx/qrRight/qrX/qrY/availW/textY` and `logoBmp` at declaration with
an 8-line comment explaining that `-Os` on RISC-V demands it
([:752-763](../targets/esp32-idf/src/boot_screen.cpp),
[:869-875](../targets/esp32-idf/src/boot_screen.cpp)). Nordic leaves all of them uninitialised
([:788-791](../targets/nordic-zephyr/src/boot_screen.cpp),
[:897](../targets/nordic-zephyr/src/boot_screen.cpp)). Toolchain drift only for the first group;
`logoBmp` is genuinely uninitialised on Nordic but unreachable (§ 2.3.1).

**2.1.8 The `colorPlanePass` row early-out.**

esp32 skips the whole x-loop for rows outside the swatch band on the colour plane
([:968-973](../targets/esp32-idf/src/boot_screen.cpp)); Nordic does not
([:976](../targets/nordic-zephyr/src/boot_screen.cpp)). Output-identical; a pure speed
difference on a path that already scans W×H twice.

### 2.2 Deliberate adaptation (b)

**2.2.1 Data access shape.** esp32 links four globals —
`extern struct od_config globalConfig; extern struct SecurityConfig securityConfig; extern
BBEPDISP bbep; extern uint8_t staticRowBuffer[BOOT_ROW_BUFFER_SIZE];` —
[:21-24](../targets/esp32-idf/src/boot_screen.cpp). Nordic goes through accessors
`boot_cfg()` / `boot_sec()` ([:32-40](../targets/nordic-zephyr/src/boot_screen.cpp)), takes the
panel as a parameter (`writeBootScreenWithQr(BBEPDISP &epd)`,
[:613](../targets/nordic-zephyr/src/boot_screen.cpp)), and owns its own buffers
([:29-30](../targets/nordic-zephyr/src/boot_screen.cpp)).
**Nordic's is already the shape a shared module needs.**

**2.2.2 FastEPD.** ESP32 alone has a second panel backend, and it is `#if
defined(OPENDISPLAY_FASTEPD)`-gated in five places:
[:561-565](../targets/esp32-idf/src/boot_screen.cpp) (`direct2bpp` swatch codes),
[:932-936](../targets/esp32-idf/src/boot_screen.cpp) (`gray4Split` suppression),
[:948-956](../targets/esp32-idf/src/boot_screen.cpp) (skip `bbepSetAddrWindow`/`bbepStartWrite`),
[:1021-1035](../targets/esp32-idf/src/boot_screen.cpp) (`fastepd_boot_write_row`),
[:1039-1043](../targets/esp32-idf/src/boot_screen.cpp) (`fastepd_boot_skip_planes`). Genuine
adaptation and the strongest argument that the row sink must be an indirection, not a direct call.

**2.2.3 Bits-per-pixel.** esp32 calls `getBitsPerPixel()`
([display_service.cpp:1549-1562](../targets/esp32-idf/src/display_service.cpp)), which carries a
FastEPD-only `ED103TC2_1872X1404_4GRAY` override and knows `OD_COLOR_SCHEME_BWGBRY_SPLIT`. Nordic
calls `opendisplay_color_bits_per_pixel()`
([opendisplay_display_color.c:27-39](../targets/nordic-zephyr/src/opendisplay_display_color.c)),
which has neither. Adaptation for the override; drift for `BWGBRY_SPLIT`.

**2.2.4 Plane selection.** esp32 declares `int getplane();`
([:32](../targets/esp32-idf/src/boot_screen.cpp)), defined at
[display_service.cpp:1541-1547](../targets/esp32-idf/src/display_service.cpp). Nordic inlines the
**identical** body as `boot_get_plane()`
([:67-79](../targets/nordic-zephyr/src/boot_screen.cpp)). Same code, two homes.

**2.2.5 The gray4 plane scratch.** esp32 aliases scratch into the tail of the shared row buffer,
`uint8_t* planeRow = staticRowBuffer + pitch2bpp;`
([:515-516](../targets/esp32-idf/src/boot_screen.cpp)), and guards the total at
[:938-941](../targets/esp32-idf/src/boot_screen.cpp) (`return false` if it will not fit). Nordic
uses a dedicated `static uint8_t s_gray4_plane_scratch[(2720 + 7) / 8]` (340 B)
([:30](../targets/nordic-zephyr/src/boot_screen.cpp),
[:572-573](../targets/nordic-zephyr/src/boot_screen.cpp)) and has **no** overflow guard. Two
correct answers to the same hazard, with opposite costs (ESP32: refuse; Nordic: 340 B of RAM).
Note the esp32 comment at [:514](../targets/esp32-idf/src/boot_screen.cpp) says *"well within the
680B buffer"* while `BOOT_ROW_BUFFER_SIZE` is actually **960**
([targets/esp32-idf/src/structs.h:17](../targets/esp32-idf/src/structs.h)) — a stale comment,
harmless, but it means Nordic's 680 B buffer is *smaller* than ESP32's and only survives because
its scratch is separate.

**2.2.6 Empty-config guard.** Nordic returns false when
`cfg == NULL || cfg->display_count == 0u` ([:614-617](../targets/nordic-zephyr/src/boot_screen.cpp)).
ESP32 has no equivalent and indexes `globalConfig.displays[0]` unconditionally
([:572-574](../targets/esp32-idf/src/boot_screen.cpp)).

### 2.3 Capability differences (c)

**2.3.1 Nordic renders NO LOGO, and this is almost certainly unintended.**

ESP32 gates the logo on `#if __has_include("logo_bitmap.h")` and defines `BOOT_HAS_LOGO`
([:13-16](../targets/esp32-idf/src/boot_screen.cpp)), then uses it at
[:868-898](../targets/esp32-idf/src/boot_screen.cpp) (scale selection, placement),
[:906-908](../targets/esp32-idf/src/boot_screen.cpp) (`headerTextMaxX = logoX - pad`) and
[:1009-1012](../targets/esp32-idf/src/boot_screen.cpp) (the pixel predicate).

Nordic has the **same three `#ifdef BOOT_HAS_LOGO` blocks**
([:896](../targets/nordic-zephyr/src/boot_screen.cpp),
[:929](../targets/nordic-zephyr/src/boot_screen.cpp),
[:1012](../targets/nordic-zephyr/src/boot_screen.cpp)) but **never includes `logo_bitmap.h` and
never defines `BOOT_HAS_LOGO`**. A repo-wide grep for `BOOT_HAS_LOGO` returns exactly six hits —
three in each file — and the only `#define` is ESP32's. A grep for `logo_bitmap` outside the
generator script returns only ESP32's `__has_include`.

Consequences on Nordic:
- No logo is drawn, on any board, ever.
- `headerTextMaxX` stays at `w_log - pad` ([:888](../targets/nordic-zephyr/src/boot_screen.cpp))
  instead of `logoX - pad`, so the manufacturer/model header is scaled to the **full** width — a
  layout difference, not just a missing graphic.
- `targets/nordic-zephyr/src/logo_bitmap.h` is 111,510 B of source and 18,464 B of data that
  contributes nothing.
- `logoBmp` at [:897](../targets/nordic-zephyr/src/boot_screen.cpp) is declared uninitialised
  inside dead code.

The `__has_include` guard was dropped when the file was ported into `Firmware_NRF54`; the same
gap is present in `../Firmware_NRF54/src/boot_screen.cpp` (the 83-line diff between it and the
unified copy contains nothing logo-related). **This is a live defect independent of any
de-duplication**, and de-duplicating would fix it as a side effect — which means the merge
changes what a Nordic board displays and must be hardware-verified, not just built.

**2.3.2 `OD_COLOR_SCHEME_BWGBRY_SPLIT` (value 8) is ESP32-only.** ESP32 handles it in the label
switch ([:317](../targets/esp32-idf/src/boot_screen.cpp)), the white-value table
([:541](../targets/esp32-idf/src/boot_screen.cpp), `0x11`) and the swatch switch
([:641](../targets/esp32-idf/src/boot_screen.cpp)). Nordic's `kSchemeWhiteValue[]` has only 8
entries ([:582-591](../targets/nordic-zephyr/src/boot_screen.cpp)), so scheme 8 falls through the
bounds check at [:644-646](../targets/nordic-zephyr/src/boot_screen.cpp) to `0xFF` — the wrong
white for a 4 bpp scheme — and draws no swatches. Unreachable on Nordic hardware today, but it is
a **silent** wrong answer rather than a refusal.

**2.3.3 Split-panel (E1004 dual controller) and watchdog breadcrumbs exist only upstream.** See
§ 2.4.2 / § 2.4.3.

### 2.4 The upstream axis: `../Firmware` vs the `esp32-idf` snapshot (173 `diff -w` lines)

Per CLAUDE.md, `../Firmware/` is the authority and `targets/esp32-idf/src/` is a drifting
snapshot. Three findings:

**2.4.1 Both unified copies are behind a shipped upstream bug fix.**

`../Firmware` commit `64184bb` — *"fix(boot): fill the boot-screen swatches on BWR/BWY panels
(#149)"*, 2026-08-10 — changed `} else if (!useBitplanes) {` to `} else {` at
`../Firmware/src/boot_screen.cpp:1022`. The commit message: *"on a bitplane scheme the black one
rendered as an empty outlined box."*

Both unified copies still carry the buggy guard:
[esp32-idf boot_screen.cpp:1016](../targets/esp32-idf/src/boot_screen.cpp) and
[nordic-zephyr boot_screen.cpp:1018](../targets/nordic-zephyr/src/boot_screen.cpp). Any BWR or
BWY board flashed from this repo shows an empty black swatch. **Fix this before or during the
merge; do not carry it forward.**

**2.4.2 Split-panel support was dropped at import.** `../Firmware/src/boot_screen.cpp` has
`splitPanelUsed()` / `splitPanelBeginFrame()` / `splitPanelSinkBytes()` and a `halfPass` loop that
paints and emits the left half-plane then the right (upstream lines ~909-917, ~933-941, ~965-972,
~1032-1044, plus a comment explaining that the frame is deliberately *not* closed there). None of
it survives in `targets/esp32-idf/src/boot_screen.cpp`. Landed upstream in `20807f3`
(*"E1004 / dual-controller panels…"*). Whether the omission is a deliberate Firmware_Unified
adaptation or import drift is **UNVERIFIED** — but a shared renderer must decide, because
half-plane emission is a *sink* concern and directly shapes the seam (§ 4).

**2.4.3 Watchdog breadcrumbs were dropped.** `odWatchdogBreadcrumb(OD_WDT_PHASE_STREAM)` appears
four times in `../Firmware/src/boot_screen.cpp` (upstream :515, :1035, :1044, :1066) and zero
times in either unified copy. `od_watchdog` *is* promoted (`shared/core/od_watchdog.c`), so this
is recoverable inside shared code — the boot screen is a multi-second W×H×2 scan and is exactly
the kind of loop a TWDT is for.

**2.4.4 Everything else in the 173 lines is legitimate adaptation** — `<Arduino.h>` and `String`
removal, `struct GlobalConfig` → `struct od_config`, `#if defined(TARGET_ESP32) &&
defined(OPENDISPLAY_FASTEPD)` → `#if defined(OPENDISPLAY_FASTEPD)`, the
`-Werror=maybe-uninitialized` initialisers, and the `logoBmp != nullptr` null check that upstream
lacks ([esp32-idf:1010-1011](../targets/esp32-idf/src/boot_screen.cpp)).

`../Firmware_NRF54` → `nordic-zephyr` (83 lines) is **entirely** adaptation: `struct GlobalConfig`
→ `struct od_config`, `BBEPAPER` C++ methods → `bbep*()` C functions, `SECURITY_FLAG_*` →
`OD_SECURITY_FLAG_*`, `tag_type` → `legacy_tag_type`, `printf` → `od_log_info`, and the
`od_hwinfo_get_device_id()` fold replaced by `opendisplay_ble_chip_id_last24()`. No drift to
reconcile on that side.

---

## 3. The third implementation: `efr32bg22-slc`

`render_boot_screen(BBEPDISP&, const struct GlobalConfig*)` —
[opendisplay_display.cpp:350-560](../targets/efr32bg22-slc/opendisplay_display.cpp), reached
through `opendisplay_display_boot_apply()`
([:688-710](../targets/efr32bg22-slc/opendisplay_display.cpp)) behind a one-shot `s_boot_applied`
latch ([:37](../targets/efr32bg22-slc/opendisplay_display.cpp)).

It is **not** a subset of the other two — it is an independent derivation that happens to produce
the same landing URL.

### 3.1 What it shares, semantically

- The same 23-byte QR payload layout: tag type BE, 24-bit chip id, 16-byte key or zeros,
  manufacturer id BE — [:387-403](../targets/efr32bg22-slc/opendisplay_display.cpp) vs
  [esp32:666-685](../targets/esp32-idf/src/boot_screen.cpp) and
  [nordic:702-716](../targets/nordic-zephyr/src/boot_screen.cpp).
- The same `https://opendisplay.org/l/?` prefix
  ([:352](../targets/efr32bg22-slc/opendisplay_display.cpp)) and the same QR v6 / ECC_MEDIUM
  ([:410-416](../targets/efr32bg22-slc/opendisplay_display.cpp)).
- A near-identical `base64url_encode()` ([:311-348](../targets/efr32bg22-slc/opendisplay_display.cpp))
  and `bytes_to_hex()` ([:297-309](../targets/efr32bg22-slc/opendisplay_display.cpp)).
- The same `boot_line_step(scale) = scale * 10`
  ([:217-220](../targets/efr32bg22-slc/opendisplay_display.cpp)) and the same
  `text_width_px = strlen * 6 * scale` ([:209-215](../targets/efr32bg22-slc/opendisplay_display.cpp)).
- A `boot_layout_fit()` with the same landscape/portrait shape
  ([:240-295](../targets/efr32bg22-slc/opendisplay_display.cpp)).
- `struct GlobalConfig` is `#define GlobalConfig od_config`
  ([opendisplay_runtime.h:26](../targets/efr32bg22-slc/opendisplay_runtime.h)) — **all three
  targets already speak `struct od_config`.** The config input to a shared renderer needs no
  translation layer.

### 3.2 What it lacks (capability, forced by the part)

- **Font: 30 glyphs, not 95** — `s_font5x7[]`
  ([:138-154](../targets/efr32bg22-slc/opendisplay_display.cpp)) covers space, `.`, `:`, digits,
  and only the uppercase letters its five fixed strings need. 180 B rodata vs 570 B.
- **No rotation.** No `w_log`/`h_log`; it renders in native orientation only.
- **No zone layout** — no header, no footer, no divider rules, no manufacturer/model lines, no
  battery/temperature, no resolution or scheme labels, no colour swatches.
- **No logo.**
- **QR module cap hardcoded to 6** ([:254-256](../targets/efr32bg22-slc/opendisplay_display.cpp))
  vs the panel-size table `bootQrModuleMax()`
  ([esp32:335-343](../targets/esp32-idf/src/boot_screen.cpp)).
- **Text scale capped at 2** ([:441](../targets/efr32bg22-slc/opendisplay_display.cpp)) vs up to
  10 ([`bootMiddleScaleHi`, esp32:346-352](../targets/esp32-idf/src/boot_screen.cpp)).
- **Different pixel format detection**: `is4clr`/`is3clr` come from bb_epaper's `BBEP_4COLOR` /
  `BBEP_3COLOR` flags ([:467-468](../targets/efr32bg22-slc/opendisplay_display.cpp)), **not** from
  `color_scheme`. There is no 4 bpp path, no gray4 plane split, no Spectra6.
- **Different text alignment**: BG22 centres each line within `avail_w`
  ([:485-489](../targets/efr32bg22-slc/opendisplay_display.cpp)); ESP32/Nordic left-align all
  lines at a common `textOriginX`
  ([esp32:849-856](../targets/esp32-idf/src/boot_screen.cpp)).
- **Different rasteriser shape**: BG22 iterates glyph → column → scale and *writes* pixels
  (`draw_text_row`, [:176-207](../targets/efr32bg22-slc/opendisplay_display.cpp)); ESP32/Nordic
  iterate pixel → *ask* each element `is this pixel black?`
  (`bootTextPixelBlack`, [esp32:220-242](../targets/esp32-idf/src/boot_screen.cpp)). The
  ask-per-pixel form is what makes rotation and clipping cheap; the write form is what makes the
  30-glyph font enough.

### 3.3 What it does differently in geometry

`boot_layout_fit()` pins content to `pad` rather than centring it
([:268-272](../targets/efr32bg22-slc/opendisplay_display.cpp)) and puts the portrait QR at
`h - pad - qr_px` ([:288](../targets/efr32bg22-slc/opendisplay_display.cpp)), where ESP32/Nordic
centre the combined block ([esp32:476-504](../targets/esp32-idf/src/boot_screen.cpp)). Same
solver skeleton, different placement policy.

### 3.4 A user-visible BG22 divergence worth fixing regardless

BG22 zeroes its local `key` buffer and only fills it when the flag is set
([:396-399](../targets/efr32bg22-slc/opendisplay_display.cpp)), then unconditionally hexes
*that* buffer into the on-screen `k1`/`k2`
([:427-431](../targets/efr32bg22-slc/opendisplay_display.cpp)). So on BG22:

- key not set → the screen shows **32 zeros**
- key set but `SHOW_KEY_ON_SCREEN` clear → the screen shows **32 zeros**

ESP32/Nordic distinguish the two: `formatBootKeyDisplay()` writes 32 `-` for "not set" and 32 `X`
for "hidden" ([esp32:147-162](../targets/esp32-idf/src/boot_screen.cpp)), and the zone layout
writes `"KEY1: not set"` / `"KEY1: hidden"`
([esp32:164-175](../targets/esp32-idf/src/boot_screen.cpp)). **A BG22 user cannot tell an
unprovisioned tag from a provisioned one with key display disabled.** This is exactly the class of
policy that should exist once.

---

## 4. The proposed seam

### 4.1 The drawing surface — the question that has to be answered first

Three candidates; only one works.

**Not a caller-supplied framebuffer span.** An 800×480 mono frame is 48,000 B. BG22 has 32 KB of
RAM *total*. The row-at-a-time design in all three implementations exists *because* there is no
framebuffer. Rejected on arithmetic.

**Not `od_hal_panel` as it stands.** `targets/esp32-idf/hal/od_hal_panel.h` is the right *idea*
and is explicitly written to be promotable, but three things block it today:

1. It lives in `targets/esp32-idf/hal/`, not `shared/hal/`, and **only ESP32 implements it**.
   Adopting it as the boot-screen surface makes the boot de-dup depend on porting the panel HAL to
   two more targets — a far larger project.
2. `od_hal_panel_begin()` opens a full-frame session with **no plane argument**, and the header
   normalises plane order to "plane 0 is the OLD plane" (correction 5). The boot renderer needs
   the opposite: explicit, scheme-driven plane targeting — `PLANE_0` for BWR/BWY's B/W plane,
   `PLANE_1` for the colour plane, both for the 4-gray split, and `getplane()`'s scheme-dependent
   choice otherwise ([esp32:942-956](../targets/esp32-idf/src/boot_screen.cpp),
   [nordic:958-973](../targets/nordic-zephyr/src/boot_screen.cpp)). The existing contract cannot
   express "start writing the colour plane now."
3. The FastEPD arm bypasses `bbepSetAddrWindow`/`bbepStartWrite` entirely and uses
   `fastepd_boot_write_row(y, row, pitch)` — a *positional* row write, not a stream
   ([esp32:1021-1023](../targets/esp32-idf/src/boot_screen.cpp)). Any surface must carry `y`.

**The answer: a link-time seam header, `shared/core/od_boot_app.h`.** This is architectural
decision 1's stated pattern — *"every interface across the boundary is a link-time `extern` C
function the target implements"* — and it is precisely how `od_cmd_app.h`, `od_session_app.h` and
`od_rxq_app_report` already work. It is **not** a second vtable, so decision 2 is untouched.

```c
/* shared/core/od_boot_app.h — the boot renderer's target seam.
 * Plane identity is normalised here; targets map to bb_epaper's PLANE_0/PLANE_1. */
#define OD_BOOT_PLANE_PRIMARY  0   /* mono / packed / BWR-BWY B&W / gray4 LSB */
#define OD_BOOT_PLANE_SECOND   1   /* BWR-BWY colour / gray4 MSB */

/* Open a plane for streaming. Called once per pass, before any row. */
int  od_boot_app_begin_plane(int plane, uint16_t w, uint16_t h);
/* One packed native row. y is supplied because FastEPD writes positionally. */
int  od_boot_app_write_row(uint16_t y, const uint8_t *row, uint16_t len);
/* Close the pass. The E1004 split panel's deferred close lives behind this. */
void od_boot_app_end_plane(int plane);

/* Facts the renderer cannot derive. Each is one target function. */
int   od_boot_app_bits_per_pixel(void);   /* FastEPD's ED103TC2 override lives here */
int   od_boot_app_default_plane(void);    /* getplane() / boot_get_plane() */
bool  od_boot_app_direct_2bpp(void);      /* fastepd_driver_used(): gray4 without the split */
void  od_boot_app_device_id_hex(char out[7]);
void  od_boot_app_firmware_version(uint8_t *maj, uint8_t *min, uint8_t *patch);
float od_boot_app_battery_volts(void);    /* <0 = unknown */
float od_boot_app_chip_temp_c(void);      /* <-900 = unknown */
```

and the entry point:

```c
/* shared/core/od_boot_screen.h */
bool od_boot_screen_render(const struct od_config *cfg,
                           const struct SecurityConfig *sec,
                           uint8_t *row_scratch, size_t row_scratch_len);
```

**The row buffer is caller-supplied and caller-sized.** This is the single most important
memory decision: BG22 passes a 256 B buffer, Nordic 680 B, ESP32 960 B. The renderer refuses
(`return false`) when `row_scratch_len` cannot hold `pitch` — or `pitch + planePitch` when the
gray4 split is active, which is ESP32's existing guard at
[:938-941](../targets/esp32-idf/src/boot_screen.cpp) promoted, and which makes Nordic's separate
340 B `s_gray4_plane_scratch` unnecessary. **Refuse, never truncate** — the same rule CLAUDE.md
decision 12 applies to `OD_CONFIG_MAX_SIZE`.

### 4.2 What `shared/core/od_boot_screen.c` owns

Everything below is currently duplicated character-for-character between the two `.cpp` files:

| Piece | esp32 lines | nordic lines |
|---|---|---|
| `BOOT_FONT5X7[]` + `bootGlyph()` — 95 glyphs | 37-93 | 106-162 |
| `base64UrlEncode()` | 95-124 | 164-193 |
| `bytesToHex()` | 126-138 | 195-207 |
| key redaction: `bootKeyIsAllZero`, `formatBootKeyDisplay`, `bootFormatKeyLine` | 140-175 | 209-241 |
| pixel packers `setBootPixelBlack` / `setBootPixelCode` | 177-218 | 243-284 |
| element predicates: text / QR / logo | 220-265 | 286-331 |
| metrics: `bootTextWidth`, `bootLineStep`, `bootMaxTextWidth` | 267-307 | 333-373 |
| swatch geometry `bootSwatchIndex` / `bootSwatchIsBorder` | 275-297 | 341-363 |
| `formatBootColorSchemeText` | 309-331 | 375-388 |
| `bootQrModuleMax`, `bootMiddleScaleHi`, `bootMiddlePad` | 333-357 | 390-414 |
| footer segment layout | 360-383 | 417-440 |
| header scale solver `bootHeaderLineGap/BlockH/PickHeaderScales` | 385-453 | 442-510 |
| `bootLayoutFit` | 455-508 | 512-565 |
| gray4 plane de-interleave `writeGray4PlaneRow` | 515-525 | 572-580 |
| `kSchemeWhiteValue`, `kBwgbrySwatchCodes`, `kGray4Stored*`, `bootGray4FillSwatchCodes` | 527-569 | 582-611 |
| the QR payload + URL build | 663-698 | 698-730 |
| the string set (domain/id/fw/key lines) | 704-747 | 736-783 |
| the layout drive loop | 749-814 | 785-842 |
| the footer/swatch band placement | 816-926 | 844-949 |
| the rotation map + row composition loop | 943-1037 | 958-1029 |

**That is essentially the whole of both files.** What is left target-side after the extraction is
under ~60 lines each.

### 4.3 What stays target-side

- The three bb_epaper / FastEPD / split-panel calls, behind `od_boot_app_*`.
- `getBitsPerPixel()`'s FastEPD panel override and `getplane()`.
- Battery / temperature / device-id / firmware-version reads.
- The row scratch buffer's storage and size.
- The refresh, busy-wait and power sequencing (BG22:
  [opendisplay_display.cpp:562+](../targets/efr32bg22-slc/opendisplay_display.cpp); ESP32:
  `refreshBootScreenFull()`,
  [display_service.cpp:457-458](../targets/esp32-idf/src/display_service.cpp)).

### 4.4 Tier and build placement

Split into **two** shared sources so BG22 can take the cheap half without the expensive half:

| Source | Tier | Rationale |
|---|---|---|
| `shared/core/od_boot_payload.c` | **PURE** | base64url, hex, key redaction policy, the 23-byte landing payload, the URL. No HAL, no seam, no target function. All three targets already agree on the bytes; this pins them. |
| `shared/core/od_boot_screen.c` | **APP_BOOT** (new tier) | Needs `od_boot_app.h`. Named for a *seam*, not a HAL — same precedent as APP_SESSION and APP_RXQ, and stated in `shared/sources.cmake`'s tier commentary. |

A new tier is required: `od_boot_screen.c` needs neither `od_hal_adv`, `od_hal_crypto`,
`od_hal_radio` nor `od_hal_wdt`, and it needs something none of the existing APP_* tiers provide.
Adding it to an existing tier would force a target to take the renderer to get the session code.

`shared/sources.cmake` composes `OD_SHARED_SOURCES` from the tiers, so both files reach the host
tests automatically. Consumers: `esp32-idf` and `nordic-zephyr` take PURE + APP_BOOT;
`efr32bg22-slc` takes PURE unconditionally, and APP_BOOT only if § 5.3 is resolved in its favour.

### 4.5 The QR encoder and the logo asset

Neither belongs in `shared/`.

- **`qrcode.c/h` → `third_party/qrcode/`.** It is vendored MIT code, exempt from the one rule by
  decision 13, and the three copies are functionally identical (§ 1). Adopt the Nordic/Silabs
  byte-identical pair as canonical; ESP32's is the same code reformatted. One copy, three
  consumers, exactly as `third_party/bb_epaper` already works.
- **`logo_bitmap.h` → one generated asset, one home** (`third_party/` or an `assets/`
  directory), with `tools/convert_logo.py` moved out of `targets/esp32-idf/tools/` alongside it.
  It is a generated artefact, not shared *logic*, and putting a 111 KB header under `shared/core/`
  would be misleading. Whichever home it gets, its inclusion must stay compile-gated —
  `OD_BOOT_LOGO_ENABLE`, defaulting on where flash allows and **off on BG22** (§ 5.2).

### 4.6 Testability — the strongest argument for doing this

The renderer is a pure function of (geometry, facts) → rows. A host test can implement
`od_boot_app_*` over a `malloc`'d frame, render at every scheme × rotation × representative panel
geometry, and pin a hash per case. Nothing in the current arrangement is testable at all: `tools/check.sh`
has no boot-screen check, and neither `../Firmware/tools/` nor `tests/host/` contains one
(**UNVERIFIED** for `../Firmware/tools/` — I did not enumerate it exhaustively).

That matters more than usual here because § 2.4.1 is a *rendering* bug that shipped and was found
by eye on hardware, and § 2.3.1 is a *missing element* nobody noticed across two repos and an
import. Both are exactly what a golden-image host test catches for free.

---

## 5. Memory analysis

### 5.1 The logo asset

From [logo_bitmap.h](../targets/esp32-idf/src/logo_bitmap.h): three pre-rasterised scales,
`static const uint8_t`, so `.rodata` (flash) — never RAM.

| Symbol | Dimensions | Stride | Bytes |
|---|---|---:|---:|
| `BOOT_LOGO_BITMAP_S1` (:5-8) | 84 × 44 | 11 | 484 |
| `BOOT_LOGO_BITMAP_S2` (:42-45) | 154 × 80 | 20 | 1,600 |
| `BOOT_LOGO_BITMAP_S3` (:148-151) | 499 × 260 | 63 | 16,380 |
| **total** | | | **18,464** |

The asset is duplicated **four times in the workspace** (two of them in this repo) and is
**byte-identical** in all four — md5 `bb6250d2a8dd9912fb1845ed8a41d53d`. Exactly **one** copy is
ever compiled: ESP32's. Nordic's 111,510 B header is dead (§ 2.3.1).

**BG22 must not take the logo.** 18,464 B against a 249,796 B image (CLAUDE.md, C13) is a 7.4 %
flash increase for a decorative element on a part whose header zone does not exist. Even the S1
scale (484 B) requires the zone layout BG22 does not have. Gate it off and say so in the build.

### 5.2 Font and static tables

| Table | esp32/nordic | BG22 today |
|---|---:|---:|
| 5×7 font | 95 entries × 6 B = **570 B** | 30 entries × 6 B = **180 B** |
| `kSchemeWhiteValue` | 9 B / 8 B | — |
| `kBwgbrySwatchCodes` + `kGray4Stored{Base,V2}` | 14 B | — |

Adopting the shared 95-glyph font on BG22 costs **+390 B of flash**. Trivial. But note the
BG22 font is 30 glyphs *because its five strings need 30* — if BG22 later takes shared *strings*
(`"KEY1: hidden"`, `"URL:  OpenDisplay.org"`, `"+Partial Refresh"`), it needs the full font
anyway, so the two decisions are coupled.

### 5.3 RAM — the BG22 constraint, and it is the stack, not the heap

Static, per target, today:

| Target | Row buffer | Gray4 scratch | QR buffer | Static total |
|---|---:|---:|---:|---:|
| esp32-idf | 960 B (`BOOT_ROW_BUFFER_SIZE`, [structs.h:17](../targets/esp32-idf/src/structs.h), shared with `display_service`) | aliased into the row buffer | on the stack | 960 B |
| nordic-zephyr | 680 B ([:29](../targets/nordic-zephyr/src/boot_screen.cpp)) | 340 B ([:30](../targets/nordic-zephyr/src/boot_screen.cpp)) | on the stack | 1,020 B |
| efr32bg22-slc | 256 B ([:466](../targets/efr32bg22-slc/opendisplay_display.cpp)) | n/a | **256 B static** ([:363](../targets/efr32bg22-slc/opendisplay_display.cpp)) | 512 B |

The BG22 256 B row buffer is sufficient: the largest panel in
[opendisplay_epd_map.c](../targets/efr32bg22-slc/opendisplay_epd_map.c) is
`EP81_SPECTRA_1024x576` (0x002B), and 1024 px at 4-colour 2 bpp is exactly 256 B; at 1 bpp it is
128 B.

**The stack is the real ceiling.** `SL_STACK_SIZE` is **2752**
([config/sl_memory_manager_region_config.h:45](../targets/efr32bg22-slc/config/sl_memory_manager_region_config.h)).
Summing the automatic storage in `writeBootScreenWithQr()` by inspection:

| Object | esp32 line | Bytes |
|---|---|---:|
| `uint8_t qrBuf[256]` | :695 | 256 |
| `char url[128]` | :690 | 128 |
| `char payloadB64[64]` | :687 | 64 |
| `manufLine[32]`, `modelLine[32]` | :707-708 | 64 |
| `nameLine[32]`, `fwLine[32]` | :714-715 | 64 |
| `k1[24]`, `k2[24]` | :725 | 48 |
| `footerSegs[6]` + `footerSegX[6]` | :610-611 | 48 |
| `swatchCode[16]` + `swatchIsColor[16]` | :613-614 | 32 |
| `footerSchemeStr[16]` + `footerResStr[16]` | :608-609 | 32 |
| `payload[23]`, `last6[7]`, `vStr[8]`, `tStr[8]` | :663-666, :738-739 | 48 |
| `QRCode qr` + ~30 scalars | :693 | ~130 |
| **≈ total** | | **≈ 900 B** |

Estimated, not measured — no `-fstack-usage` run was performed, so treat as ±25 %.

That is **~33 % of BG22's entire stack** in one frame, before the call chain above it. The BG22
implementation makes `qr_buf` **static** on purpose
([:363](../targets/efr32bg22-slc/opendisplay_display.cpp)) — 256 B of that 900 moved out of the
stack — which is direct evidence that stack was already tight when it was written. BG22's own
frame is roughly 300–350 B by the same method.

CLAUDE.md's C13 entry records **480 B of main-RAM headroom** and an elastic heap grown from
`0x2958` to `0x2d58` (11,608 B). Those are quoted, **not measured by me**. If accurate, a 550 B
increase in peak stack on a part with 480 B of static headroom is not obviously safe, and the
answer is not "grow the stack" — it is:

1. Make `qrBuf` a caller-supplied buffer, like the row scratch, so BG22 can keep it static.
2. Make the header/footer string set compile-gated (`OD_BOOT_ZONES_ENABLE`), which removes
   `manufLine`, `modelLine`, `footerSchemeStr`, `footerResStr`, `footerSegs`, `footerSegX`,
   `vStr`, `tStr` — ~210 B — on a target that renders none of them.
3. Measure with `-fstack-usage` before committing BG22 to APP_BOOT.

### 5.4 Flash

Both `.cpp` files are ~1,050 lines of dense integer code; a merged `od_boot_screen.c` compiled
once and linked into three images **saves nothing** — each image links its own copy either way.
The flash argument for de-duplication is neutral, apart from removing 18,464 B of dead-but-present
asset from the Nordic tree (it is already excluded by the linker, so **0 B saved in the image**;
the saving is 111 KB of source).

The flash *cost* on BG22 if it adopts APP_BOOT is the delta between its ~210-line renderer and the
shared ~1,050-line one, plus 390 B of font — **UNVERIFIED**, estimate 4–7 KB of `.text`, which is
2–3 % of the 249,796 B image. Affordable; must be measured.

---

## 6. Recommendation

**Phase 1 — do it.** Merge `esp32-idf` and `nordic-zephyr` onto
`shared/core/od_boot_screen.c` (APP_BOOT) + `shared/core/od_boot_payload.c` (PURE). Adopt
`../Firmware`'s form everywhere *except* the QR-position fix (§ 2.1.1), where Nordic is right and
the exception must be written into `docs/DIVERGENCE_MATRIX.md`. Land the upstream swatch fix
(§ 2.4.1) in the same change and note that it alters what BWR/BWY boards display. Restore the
logo on Nordic (§ 2.3.1) as a *stated* behaviour change requiring hardware verification, not as an
incidental consequence.

**Phase 2 — decide separately.** BG22 takes `od_boot_payload.c` immediately: it is PURE, it costs
nothing, it settles the key-redaction policy (§ 3.4), and it pins the one part of the boot screen
with an external consumer (the opendisplay.org landing service). Whether it takes
`od_boot_screen.c` depends on a measured stack number, not on a principle.

**What I would not do:** merge all three in one change, or make the BG22 boot screen a
compile-time subset of a renderer designed for 1872×1404 panels with three-zone layouts. That is
the "lowest-common-denominator features" failure mode decision 9 exists to prevent, run in
reverse.

---

## 7. Open questions, ranked

**1. Is the split-panel (E1004) path a deliberate Firmware_Unified omission or import drift?**
`../Firmware/src/boot_screen.cpp` emits the boot frame in two half-plane passes with a deferred
frame close; `targets/esp32-idf/src/boot_screen.cpp` does not (§ 2.4.2). This is first because it
**shapes the seam**: half-plane emission means `od_boot_app_write_row()` may be called with a
partial row and a deferred `end_plane`, which is a different contract from the one in § 4.1. Get
this wrong and the seam has to be redesigned after the fact. Ask whoever did the import.

**2. Does BG22 adopt the full renderer, and what is its measured peak stack?**
The § 5.3 estimate (~900 B against a 2,752 B stack) is by inspection. Run `-fstack-usage` on the
BG22 build with a prototype before committing. If the answer is "no", § 4.4's two-file split is
load-bearing and must be in the design from day one, not retrofitted.

**3. Which QR position is correct — `qrPx` (Nordic) or `modulePx * (qrSize + quiet)` (ESP32 and
`../Firmware`)?** Nordic's comment claims the ESP32 form clips the right edge (§ 2.1.1). This is
the one place the CLAUDE.md authority rule points at the wrong answer, so it needs an explicit
override with evidence — ideally a photograph of a landscape panel under both forms, since a
clipped quiet zone can still scan.

**4. Does the shared renderer log the landing URL?** Nordic does
([:723](../targets/nordic-zephyr/src/boot_screen.cpp)); ESP32 does not. The URL embeds the
encryption key when `SHOW_KEY_ON_SCREEN` is set. Decide once: log always, log with the key
redacted, or never.

**5. Where does the logo live, and is it enabled per target or per board?**
Currently ESP32-only-by-accident. `#if __has_include` is a fragile gate — it is precisely what
failed on Nordic. Replace with an explicit `OD_BOOT_LOGO_ENABLE` set by each build, and decide
whether the 18,464 B S3 bitmap earns its place on a 256 KB nRF52840 (it does) and on BG22 (it does
not).

**6. Does `od_boot_screen.c` feed the watchdog?** `../Firmware` breadcrumbs four times through the
scan (§ 2.4.3); neither unified copy does. `od_watchdog` is already shared, so the shared renderer
*can* — but arming widens ESP32's idle TWDT to 300 s (CLAUDE.md), and only `main()` feeds on
Nordic, so a breadcrumb in the boot scan interacts with both. Decide before writing the loop.

**7. Does `qrcode.c` move to `third_party/`, and which copy is canonical?** The three are
functionally identical; Nordic's and Silabs' are byte-identical. Low-risk, but it is a
three-consumer move that touches three build files and should not be smuggled in with the
renderer.

**8. Does BG22 keep its own layout policy after taking shared code?** Its text is centred and
pinned to `pad`; ESP32/Nordic left-align and centre the block (§ 3.3). If BG22 takes
`od_boot_screen.c`, its boot screen *changes appearance*. Acceptable, but it is a product decision
and someone should say so out loud.

**9. Where do the logo source and its generator live once the asset moves?**
`targets/esp32-idf/tools/convert_logo.py` defaults to `tools/od_logo.svg`
([:8](../targets/esp32-idf/tools/convert_logo.py), output default `src/logo_bitmap.h` at
[:115](../targets/esp32-idf/tools/convert_logo.py)), and
`targets/esp32-idf/tools/od_logo.svg` is present — so the bitmaps *are* regenerable. But the SVG
and the script currently sit inside one target's tree while the output is consumed as a
cross-target asset. If the asset moves (§ 4.5), the generator and its input move with it, or the
next regeneration silently writes to the wrong place.
