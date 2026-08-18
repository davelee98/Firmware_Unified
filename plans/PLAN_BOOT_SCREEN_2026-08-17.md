# Plan: unify the boot screen renderer

**Status:** IMPLEMENTED WITH STOP-GATE OUTCOMES, 2026-08-18, on
`feat/boot-screen-shared`. Hardware verification remains open.

**Source survey:** [DEDUP_1_BOOT_SCREEN_2026-08-17.md](DEDUP_1_BOOT_SCREEN_2026-08-17.md). This
revision retains its duplication analysis but supersedes its implementation ordering and memory/
transport assumptions where the evidence below says so.

**Re-verified against this repo at `b12e5b4`.** The extraction authority is pinned to
`../Firmware` commit **`64184bbecc88a2d07332f6f28fc922f581619ffc`** (short `64184bb`), whose
`src/boot_screen.cpp` contains the 2026-08-10 swatch fix. The sibling worktree may be on another
branch; implementation reads that exact blob, not whichever file happens to be checked out.

### Implementation result

The implementation followed the plan's safety gates rather than forcing every proposed adoption:

| Area | Result |
|---|---|
| QR encoder | Three target copies collapsed into `third_party/qrcode/`. The differently formatted ESP32 and canonical objects were compiled with identical size/section flags, stripped, and proved byte-identical. |
| Payload | `shared/core/od_boot_payload.c` is in PURE and used by all three targets, with host coverage for payload, URL, key display and redaction. |
| Renderer | `shared/core/od_boot_screen.c` is in the new APP_BOOT tier. ESP32 and Nordic now use thin target seams and one renderer; their target-local renderers and duplicate logo headers are retired. |
| Assets | The generated logo and conversion tool live together in `third_party/boot_logo/`; `OD_BOOT_LOGO_SIZES` removes unreachable sizes at compile time. |
| Lifecycle | The fallible frame/plane/row seam and one-/two-segment host contract landed. Real dual-CS transport did **not** land because no split-panel hardware was available for C-B3's required proof. ESP32 and Nordic therefore reject split panels and report one segment. |
| BG22 renderer | C-B6 was exercised and declined: linking APP_BOOT with S1+S2 exceeded BG22 flash by **3,772 B**. BG22 retains its compact renderer and adopts the shared payload and QR encoder. Its row buffer remains exactly **256 B**; fitting GRAY4 rows are re-rendered into two 1bpp controller planes using packed-row plus plane scratch inside that buffer. Scheme-based workspace overflow returns before output, silently, with no chunk/span fallback. |
| BG22 dual CS | `OD_CAP_DUAL_CS=0` is enforced as product policy. A vendor `BBEP_SPLIT_BUFFER` panel is refused silently before panel I/O. |
| Characterization | The proposed C-B1 pre-change goldens for all four renderers and generated BG22 map census did **not** land. Permanent coverage starts at the extracted payload/renderer contracts, including pixel-level authority swatch checks, V2-LUT and direct-2bpp GRAY4 assertions, and the exact 256/257-byte row boundary. This is residual test debt, not evidence that was run. |

Build evidence, followed by representative rebuilds after the final authority color-contract
corrections:

- host GCC: **37/37** tests;
- ESP-IDF release matrix: **9/9** boards; after the final correction, `esp32-c3-N4` and
  `esp32-s3-N8R8` were rebuilt successfully;
- Nordic/Zephyr: all three boards; after the final correction, `xiao_nrf52840` was rebuilt
  successfully. Application RAM is 162,812 B (`xiao_nrf54l15`),
  165,700 B (`xiao_nrf54lm20a`) and 145,700 B (`xiao_nrf52840`);
- BG22: `.text` 249,976 B, `.data` 488 B, `.bss` 31,796 B; app flash is 250,464 B.

Clang also passes all 37 non-fuzz host tests. Its three fuzz bodies each completed 2,000
iterations, but CTest cannot report those cases passing in this ptrace environment because
LeakSanitizer cannot attach; this is an environment limitation rather than a clean Clang fuzz-gate
result.

No board was flashed during implementation. The photographs, QR scan, refresh behavior and
split-panel qualification in § 8 remain hardware debt.

---

## 1. Outcome

**The base is `src/boot_screen.cpp` at pinned `../Firmware` commit `64184bb` — not the
`esp32-idf` snapshot and not an unpinned live sibling checkout.** Owner decision, 2026-08-17, and
it is CLAUDE.md's own rule rather than a new one:
*"THE AUTHORITY IS `../Firmware/`, THE SIBLING REPO — NOT `targets/esp32-idf/src/`. That directory
is a snapshot taken at import, and upstream keeps moving."* Nordic conforms to that base, or its
difference is **ported into** it and justifies itself in writing.

Extracting from the pinned sibling blob rather than the snapshot has three consequences worth
stating, because each removes work the earlier draft had scheduled:

- the 122-line snapshot drift is **carried in by construction**, so B2 stops being a port and
  becomes a property of the base;
- the split-panel half-plane loop **comes with the source**, but cannot be enabled until both
  target transports pass the prerequisite in § 2.3;
- `esp32-idf`'s swap becomes a genuine behaviour change to verify, not a no-op refactor — it gains
  everything upstream landed since the import.

`esp32-idf` and `nordic-zephyr` render the boot screen from one shared source. Five recorded
defects close, several with user-visible or hardware-visible consequences. The renderer becomes
the first subsystem in this repo with a **golden-output host test**, which is the real prize: it is
a pure function of (geometry, facts) → packed rows, and nothing about it is testable today.

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

## 2. The four decisions

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

BG22's current renderer omits zones, footer, swatches and logo. That is a description of current
behaviour, not a shared-renderer capability declaration. If BG22 adopts APP_BOOT it follows the
shared geometry decisions and therefore gains those elements where the layout selects them.

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

The earlier 13-panel census was incomplete and is withdrawn. The map contains additional native
geometries, including `EP7_960x640`; the symbol `EP1085_1360x480` is also misleading because its
vendored table entry is 680×480. C-B1 therefore generates a machine-checked census by joining
every non-undefined `opendisplay_map_epd()` result to the vendored native width, height and flags,
then evaluates all four rotations and every supported colour scheme. No acceptance decision may
use dimensions parsed from an enum name.

The relevant bounds are already verified: after refusing the 1024×576 split panel, the widest
single-controller BG22 map entry is the 960×640 `EP7_960x640`; many mapped panels meet the
400×300 zone predicate. Therefore hardcoding the current absence would freeze an implementation
gap into a target capability. The generated census supplies the exact count and remains as a host
test so a future map or vendor-table change must update the recorded 256-byte outcome and logo gate.

> **The rule: a target follows the shared layout math. A compile-time gate is only legitimate
> where the math can never select the thing gated out.**

That distinction is what separates the two gates this plan does keep from the two it just dropped:

| Gate | Legitimate? | Why |
|---|---|---|
| `OD_BOOT_ZONES_ENABLE` | **no — dropped** | mapped BG22 geometries satisfy the shared predicate |
| `OD_BOOT_SWATCH_ENABLE` | **no — dropped** | same; swatches live in a footer the math asks for |
| `OD_BOOT_LOGO_SIZES=2` | **yes** | S3 needs `w_log >= 1664`; BG22's largest non-split native dimension is 960 px. The math can never select it |
| caller-supplied `qr` buffer | **yes** | not a feature gate at all — the same bytes, in static instead of stack |

The logo gate survives precisely *because* it follows the math rather than overriding it, and the
same arithmetic independently confirms the § 3.3.2 choice: mapped 800×480 panels give
`w_log = 800 >= 514` and `headerH = 96 >= 96`, so **S2 is reachable on BG22 and S1 alone would
have been too little.**
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

**C-B6's measurement is genuinely load-bearing rather than a formality**: roughly 640 B of a
2,752 B stack, in a call chain that is not the deepest in the system.
If `-fstack-usage` says it does not fit, first reduce the frame (compose zone strings into the
existing scratch, shorten the footer segment arrays). **This plan does not raise `SL_STACK_SIZE`.**
If the measured frame still does not fit without stack growth, BG22 declines APP_BOOT and keeps
only the shared payload and dual-CS refusal.

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
larger than the boot screen and not caused by it.** It is nevertheless a prerequisite here:
C-B3 ports the bufferless lifecycle from `../Firmware` `20807f3` to ESP32 and Nordic before either
renderer may return `segments == 2`. Deferring the transport while advertising two segments would
turn a known unsupported path into an apparently supported one.

**It is also the same defect shape as B1.** A capability that disappears because a macro stopped
being defined, while every build still passes, is what both PR #147 and § 4's B1 are. That is the
argument for the rule in § 3.1: a capability must never be expressible as "defined nowhere."

### 2.4 Dual CS: ESP32 and Nordic gain verified support; BG22 excludes it by policy

Owner decisions, 2026-08-17:

- **`esp32-idf` and `nordic-zephyr` support dual CS only after C-B3 lands and passes hardware.**
  ESP32 already has held-CS primitives in `panel/od_bbep_stream.h`; Nordic has `cs_mode`/CS2
  selection but still needs the held-CS half-plane stream lifecycle. Until that work lands each
  target refuses a split panel and `od_boot_app_segments()` returns 1.
- **`efr32bg22-slc` does not support dual CS as product policy.** This is normative, not an
  implementation backlog. `shared/core/od_caps.h`, added by C13 so a target can state an absence
  that `shared/` cannot infer, records it as `OD_CAP_DUAL_CS=0` on BG22. Wiring CS2 or porting the
  split lifecycle is explicitly prohibited unless a later owner decision reverses this policy.

**Declining is not the same as ignoring, and B5 is what ignoring looks like today.** BG22 already
accepts `panel_ic_type = 0x2B` and maps it to `EP81_SPECTRA_1024x576`
(`opendisplay_epd_map.c:50`), a panel the vendored table flags `BBEP_SPLIT_BUFFER | BBEP_7COLOR`
(`bb_ep.inl:4122`). `bbepSetPanelType()` sets the flag, the library branches on it in three
places. The canonical `DisplayConfig` **does** contain `cs_pin_2`, and BG22's backend already
implements `bbepSetCS2()` plus `CMD_CS2`/`CMD_CS1_CS2` selection. Those shared/configuration and
low-level primitives do **not** advertise BG22 support and are not a reason to complete the path;
the target policy remains authoritative.

**The refusal is a runtime test on the flag, not a compile-time check on the map.** After
`bbepSetPanelType()` succeeds, BG22 tests `iFlags & BBEP_SPLIT_BUFFER` and fails the panel
configuration silently when `OD_CAP_DUAL_CS == 0`. The target emits no diagnostic, but still
refuses before panel I/O. Two reasons the runtime flag must be tested after panel selection:

1. **A macro can vanish; a flag cannot.** PR #147 exists precisely because 12 regions guarded by
   `#ifdef BBEP_T133A01` compiled to nothing when the define disappeared, while the build still
   passed. Gating on the runtime flag is the idiom upstream chose after being burned, and
   `static_assert`s on the flag value make a rename a compile error.
2. **The map is not the authority on which panels are dual-controller — the library table is.**
   Deleting `0x2B` from BG22's map would fix today's instance and miss the next panel the vendor
   flags. Testing the flag covers both.

This is the same rule as decision 12's config cap: **refuse, never truncate.** A panel excluded by
target policy should be rejected at configuration time, not half-driven.

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

**Decision: the seam carries an explicit frame lifecycle and every lifecycle operation is
fallible.** A successful frame begin is matched by frame end on every exit path; short, excess or
transport-failed streams make frame end fail and prohibit refresh. Plane begin/end remain inside
that frame. `od_boot_app_write_row()` carries `y` and segment because FastEPD writes
**positionally** while the split sink is a pure **stream**. C-B3 proves this target contract before
C-B4 extracts the renderer.

---

## 3. Architecture after

### 3.1 The seam

```c
/* shared/core/od_boot_app.h — the boot renderer's target seam.
 * Plane identity is normalised here; targets map to bb_epaper's PLANE_0/PLANE_1. */
#define OD_BOOT_PLANE_PRIMARY  0   /* mono / packed / BWR-BWY B&W / gray4 LSB */
#define OD_BOOT_PLANE_SECOND   1   /* BWR-BWY colour / gray4 MSB */

/* Open one render transaction. `segments` is 1 for an ordinary panel and 2 only after the
 * target's split transport has passed C-B3. All segment/plane output is inside this frame. */
int  od_boot_app_begin_frame(uint16_t w, uint16_t h, uint8_t segments);
int  od_boot_app_begin_plane(int plane);
/* One complete packed native row segment, pitch/segments bytes. Both `y` and `segment` are
 * supplied because positional and stream consumers need different target-side handling. */
int  od_boot_app_write_row(uint16_t y, uint8_t segment, const uint8_t *row, uint16_t len);
int  od_boot_app_end_plane(int plane);
/* Always called after a successful begin_frame, including renderer failures. Releases transport
 * state and returns failure for a short, excess or faulted frame. A failed result forbids refresh. */
int  od_boot_app_end_frame(void);

/* Facts the renderer cannot derive. One target function each. */
int   od_boot_app_bits_per_pixel(void);   /* FastEPD's ED103TC2 override */
int   od_boot_app_default_plane(void);    /* getplane() / boot_get_plane() */
bool  od_boot_app_direct_2bpp(void);      /* gray4 without the plane split */
uint8_t od_boot_app_segments(void);       /* 1, or 2 for a dual-controller panel */
uint32_t od_boot_app_device_id24(void);    /* low 24 bits, raw; PURE code formats six hex chars */
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

**Every large buffer is caller-supplied and caller-sized.** Row scratch is 960 B on ESP32, 680 B
on Nordic, and fixed at 256 B on BG22. Rendering still requires one complete packed row, plus the
GRAY4 plane scratch where applicable. `qr` remains caller-supplied and requires 256 B because
BG22 already makes it `static` deliberately to keep those bytes off a 2,752-byte stack
(`SL_STACK_SIZE`, verified at `config/sl_memory_manager_region_config.h:45`); a shared renderer
that put it back on the stack would silently undo that.

If the required row workspace exceeds `row_len`, `od_boot_screen_render()` returns `false` before
opening a frame. It emits no diagnostic. In particular, the BG22 caller does not log or substitute
another screen; the boot screen simply does not render.

#### 3.1.1 BG22 keeps its 256 B caller buffer

Owner decision, 2026-08-17: **BG22's APP_BOOT row buffer remains 256 B.** This is a hard target
memory constraint with no fallback. Any geometry/scheme combination needing more than 256 B fails
silently before frame output.

Against the renderer's pitch formula at the actual 960-pixel single-controller maximum:

| Scheme | required row workspace at 960 px | 256 B result |
|---|---:|---|
| mono / BWR / BWY | 120 B | renders |
| BWRY / 4-colour | 240 B | renders, 16 B spare |
| GRAY4 (`pitch + planePitch`) | 360 B | **silent failure** |
| BWGBRY / GRAY16 | 480 B | **silent failure** |

The fixed cap adds **zero row-buffer RAM** relative to BG22 today. The failure happens before
`od_boot_app_begin_frame()`, so it cannot truncate or leave a partially written panel even though
it is intentionally invisible to the user and logs.

#### What this makes load-bearing

Tests pin the boundary itself: a required workspace of 256 B renders; 257 B returns false with no
seam calls and no log callback. The 360 B GRAY4 and 480 B 4bpp cases above do the same. C-B1's
census records the silent render/fail result for every mapped geometry and supported scheme; a
future wider entry inherits the same fixed-cap behavior.

#### One thing not to re-derive from

The survey's sizing argument reached the right answer by the wrong route: *"the largest panel is
`EP81_SPECTRA_1024x576`, and 1024 px at 4-colour 2 bpp is exactly 256 B."* A 6-colour Spectra
configures as `BWGBRY`, which is **4 bpp**, so that panel needs 512 B. The figure landed on the
buffer size by applying the wrong bits-per-pixel to the wrong panel. Do not use that sentence to
justify the cap; use the capability table above.

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
unconditionally; APP_BOOT only if C-B6 passes.

### 3.3 The vendored neighbours

- **`qr/qrcode.c` → `third_party/qrcode/`** — see § 3.3.1, which is larger than it looks.
- **`logo_bitmap.h` → one home**, with `tools/convert_logo.py` moved beside it. It is a
  **generated asset, not shared logic**; the 111 KB of hex source under `shared/core/` would
  misrepresent what `shared/` is. Inclusion is gated **per size**, not on/off — see below.

#### 3.3.1 The QR encoder: one copy, and a differential before discarding two

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
discarding ESP32's copy. If they are *not* equivalent, that is a finding which changes what C-B4
inherits, and it must surface before the renderer extraction rather than during it.

**It lands as its own PR, sequenced first.** Not because it is separable in purpose — it is not —
but because the review question is unrelated to everything after it:

- **zero behaviour change** if the differential passes, so the PR reduces to "are these the right
  bytes"; a renderer extraction cannot be reviewed that way;
- **different governance** — `third_party/` under decision 13, not `shared/` under the one rule;
- **it stands alone.** Three copies to one is worth having even if the renderer work stalls.

#### 3.3.2 The logo gate selects SIZES, and BG22 takes S1 + S2

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

BG22's full map has no native dimension at or above 1664: its 1024×576 maximum-width entry is the
split `EP81_SPECTRA_1024x576`, which B5 refuses, and the widest remaining entry is
`EP7_960x640`. **S3 can never be selected on this target**, so excluding it removes 16,380 B that
could not have been drawn. C-B1's generated map/table census pins that bound instead of trusting
enum names or a hand-maintained panel count.

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

All five are verified in the tree, not inferred.

| # | Defect | Evidence | Closed by |
|---|---|---|---|
| **B1** | **Nordic renders no logo, silently, and has since import.** Its `boot_screen.cpp` has three `#ifdef BOOT_HAS_LOGO` blocks (`:896`, `:929`, `:1012`) and **nothing anywhere defines the macro** — ESP32 defines it at `:15`, inside the `#if __has_include("logo_bitmap.h")` guard that Nordic's copy dropped. So ~100 lines are unconditionally dead and `targets/nordic-zephyr/src/logo_bitmap.h` (111,510 B, byte-identical to ESP32's) is linked by nothing. | verified: `grep BOOT_HAS_LOGO` finds three uses and zero definitions on Nordic | one size-count gate (`OD_BOOT_LOGO_SIZES`) in shared code, so "defined nowhere" stops being representable |
| **B2** | **Both targets are behind `../Firmware`** — a swatch-fill fix landed upstream 2026-08-10, plus whatever else is in the drift. | 122 `diff -w` lines against the authority | **adopting the authority as the base closes this by construction.** It still changes what BWR/BWY boards display, so it is verified, not assumed |
| **B3** | **The authority's QR placement clips its own right quiet zone** whenever `modulePx * quiet > pad`. Nordic already fixed it. An **upstream defect**, not a Nordic divergence. | `../Firmware:820,883` vs `nordic:925` | **DECIDED 2026-08-17: adopt Nordic's form.** It is ported into the shared base, fixing ESP32 too, and reported back to `../Firmware` |
| **B4** | **BG22's key-display policy differs** in a way the survey calls arguably user-visible (§ 3.4). | survey § 3.4 | `od_boot_payload.c` settles it for all three in C-B2 |
| **B5** | **BG22 accepts a panel excluded by target policy.** Its map returns `EP81_SPECTRA_1024x576` for `panel_ic_type` `0x2B`, and the vendored table flags it `BBEP_SPLIT_BUFFER`; the current path can therefore half-drive a configuration BG22 must not accept. Existing canonical CS2 fields and backend primitives do not override the policy. | map `opendisplay_epd_map.c:50`; flag `bb_ep.inl:4122`; owner policy `OD_CAP_DUAL_CS=0` | C-B3: enforce the policy with a **runtime refusal** at panel selection (§ 2.4) |

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

**C-B4 carries `logoBmp = NULL` at the declaration and a `logoW > 0` guard at the draw site**, so
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
   `../Firmware` — the authority, and a shipping repo — still clipping. C-B4 reports it there.

**Nordic contributes exactly one line of behaviour to the merged renderer.** Every other
Nordic-only line in the 417-line `diff -w` against the authority is Zephyr plumbing, accessors and
scheme constants — the material that *becomes* the `od_boot_app_*` implementation — plus B1, which
is a defect rather than a divergence. That is a checkable claim and C-B4 should re-check it.

---

## 5. What makes this worth doing: it becomes testable

This is the strongest argument and it is not the line count.

The renderer is a pure function of (geometry, facts) → packed rows. C-B1 first wraps all three
existing target renderers and the pinned `64184bb` authority with host shims, injects fixed device id,
firmware version, battery, temperature, config and security facts, and records their output before
any visual change. Those temporary adapters disappear after extraction, but their fixtures and
hashes remain. Full-image hashes are valid only for those fixed-input fixtures; QR module matrices
and payload fields are asserted independently so volatile metadata cannot masquerade as a render
regression.

After extraction the host seam captures packed rows and pins a hash per case across scheme ×
rotation × representative native geometry. Today `tools/check.sh` has no boot-screen check of any
kind, and there is no boot-screen test in `tests/host/`.

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
| **C-B0** | `third_party/qrcode/` — one copy (the Nordic/Silabs byte-identical form), three consumers (§ 3.3.1). **Own PR, sequenced first.** | Encode the payload corpus through both implementations at every usable version/ECC and compare size plus module bits. Build both with identical flags and compare deterministic `.text`/`.rodata` contents after normalising symbols/relocations. Full boot-image equality is not the criterion. All three targets link. |
| **C-B1** | **Characterization before change.** Add fixed-fact host adapters/goldens for the pinned authority and current ESP32, Nordic and BG22 renderers. Add the generated BG22 map↔vendor-table census, including native geometry, flags, rotations, every supported scheme, row bytes and reachable logo size. No production rendering behavior changes. | Baseline hashes are committed; the authority's QR-overhang case set is non-empty; Nordic's is empty; BG22's current small/large layouts are pinned. The census proves the 960-pixel non-split bound, the fixed-256-byte silent render/fail matrix and S3 unreachability. Mutation checks show a pixel or boundary change fails. |
| **C-B2** | `shared/core/od_boot_payload.c` (PURE) + host test; all three targets swap onto it. The target supplies a raw low-24-bit device id and PURE code formats it. Settles **B4**. | Differential: payload, URL and redaction output against each target on a corpus of raw ids, keys and versions, including all-zero and all-`0xff` keys. Any intended B4 policy change has an explicit before/after fixture. |
| **C-B3** | **Dual-CS transport prerequisite.** Add `shared/core/od_boot_app.h`; port the bufferless split-frame lifecycle from `../Firmware` `20807f3` to ESP32 and Nordic and implement the frame/plane seam without changing the renderer yet. Use ESP32's existing held-CS primitive and add the equivalent Nordic primitive. Reject absent, invalid, or aliased CS2 instead of inheriting upstream's GPIO-2 default. Add `OD_CAP_DUAL_CS=0` and runtime refusal on BG22. | Host state-machine tests invoke the actual seam and cover complete, short, excess, write-failed and cleanup paths; frame end is fallible and failed frames never refresh. On actual split-panel hardware, ESP32 and Nordic each complete left then right halves and refresh. Until a target passes, it refuses split panels and cannot return `segments=2`. BG22 `0x2B` is refused silently before panel I/O. |
| **C-B4** | Add `shared/core/od_boot_screen.c` (APP_BOOT); swap ESP32. Extract from pinned `64184bb`, adopt Nordic's QR placement (B3), carry the authority swatch fix (B2), and initialise/guard `logoBmp`. Keep complete-row emission and the GRAY4 `pitch + planePitch` scratch contract. | Shared-renderer goldens match C-B1's pinned authority for fixed inputs except the separately pinned B3 QR rectangle. B2 and B3 each have enumerated before/after pixel sets. Split and ordinary frame lifecycles propagate every seam failure. |
| **C-B5** | Swap Nordic onto APP_BOOT and retire its renderer/dead logo copy. Closes **B1**. | Equal facts and geometry produce the same packed rows as ESP32. The only target-policy deltas are recorded. Hardware shows the Nordic logo and an otherwise expected boot screen; its split-panel gate from C-B3 is repeated. |
| **C-B6** | BG22 adopts APP_BOOT only if its pre-swap measurements pass. It uses `OD_BOOT_LOGO_SIZES=2`, caller-owned 256 B QR and **256 B row buffers**, the generated census, and the same runtime layout math; otherwise it retains its renderer while C-B0–C-B3 remain. | Before landing: `-fstack-usage` against `SL_STACK_SIZE=2752` with no stack growth, exact `.text`/`.data`/`.bss` deltas, every supported non-split census case matches the recorded render/silent-fail result, and hardware confirms both a fitting boot screen and an oversized case with no output or diagnostic. If optimization cannot fit, decline C-B6. |

C-B1 precedes every visual change. C-B3 precedes every renderer swap that can emit two segments.
C-B4 precedes C-B5. C-B6 is conditional and may legitimately not land.

---

## 7. Automated verification

- **Pre-change characterization**, the centrepiece: fixed-input full-frame hashes from the
  unchanged renderers land in C-B1, before the B3 behavior change or extraction. The shared
  renderer then runs every scheme × rotation × representative native geometry.
- **Deliberate diffs are enumerated, not tolerated.** B2 and B3 change ESP32 pixels; B1 adds the
  Nordic logo; B4 can change displayed key policy; C-B6 adds BG22 zones/footer/swatches/logo. Each
  has an explicit component or before/after fixture. A changed golden without an entry is failure.
- **QR evidence is component-level.** C-B0 compares encoder size/module matrices; B3 compares QR
  box coordinates and modules. A full-frame hash is used only with all metadata fixed.
- **Payload differential** (C-B2) against each target's current output, all-zero key included.
- **Fixed row-buffer boundary**: `required == row_len` renders; `required == row_len + 1` returns
  false without logging or invoking any frame/plane/write seam. BG22's 360 B GRAY4 and 480 B 4bpp
  cases are pinned silent failures at `row_len=256`. A short QR workspace also returns before output.
- **Segment and lifecycle contract**: `segments = 2` emits `2 × height` complete row segments,
  left half first; begin/end frame and plane failures propagate, and short/excess transport streams
  cannot refresh.
- **Capability truthfulness**: a target cannot report two segments until its transport is built
  and verified; otherwise configuration is refused before rendering.
- **BG22 census**: every mapped panel joins to the vendor table, native dimensions rather than enum
  names drive buffer/logo math, and unknown/duplicate/unjoined entries fail the check.
- **`tools/check.sh` gains a boot-screen check.** There is none today.

---

## 8. Hardware gates

| Row | Observation | It distinguishes |
|---|---|---|
| ESP32 ordinary-panel boot screen | photograph before/after C-B4 matches except at enumerated B2/B3 pixels | that extraction preserves the rest |
| **ESP32 split panel** | boot frame and a transport-fed frame refresh completely on a real dual-CS panel | C-B3's held-CS lifecycle and cleanup |
| **Nordic split panel** | same left/right completion and refresh on a real dual-CS panel | Nordic's new transport, not merely its existing CS2 primitives |
| **Nordic logo appears** | photographed on a flashed `xiao_nrf52840` | **B1** — it has never rendered, so this row fails by construction before C-B5 |
| Nordic boot screen otherwise unchanged | before/after photograph | that closing B1 did not disturb layout |
| QR scans on both targets | a phone resolves the landing URL | the payload path end-to-end |
| BWR/BWY swatch band | photograph on a colour panel | **B2**, which changes what these boards display |
| BG22 small and zone layouts | flashed BG22 on representative `<400×300` and `>=400×300` panels | C-B6 layout, logo and stack behavior |
| BG22 oversized row | 960px GRAY4 or 4bpp produces no boot frame and no diagnostic | fixed 256 B silent-failure policy |
| BG22 split refusal | configure `0x2B`; observe silent refusal before frame output | B5 refuses rather than half-driving |

`xiao_nrf54l15` and `xiao_nrf54lm20a` have never been flashed
([HARDWARE_MATRIX.md](../docs/HARDWARE_MATRIX.md)), so Nordic rows are `xiao_nrf52840` only and
must say so.

---

## 9. Risks and stop conditions

- **Stop before C-B4 if either split transport cannot express the fallible frame lifecycle.** A
  target without verified transport must refuse split panels and must never return two segments.
- **Stop if BG22 adoption needs the stack grown.** Optimize the frame first; if it still does not
  fit, C-B6 answers "no". This plan does not authorize a larger `SL_STACK_SIZE`.
- **Do not bundle BG22 renderer adoption before C-B6.** § 2.1.
- **Do not let a golden change land unexplained.** The whole value is that pixel changes become
  visible; a test updated to match new output is the failure mode this replaces.
- **Pinned `../Firmware` `64184bb` is the renderer authority except at B3.** Never read the live
  sibling path without verifying its blob. The B3 exception is written into `DIVERGENCE_MATRIX`
  with arithmetic and C-B1 fixtures.
- **Re-measure the pinned upstream diff before porting.** 122 lines at plan time vs the survey's
  173; the set, not the count, is what C-B4 must carry. Updating the authority pin is a reviewed
  plan change, not an incidental consequence of the sibling worktree moving.
- **The logo asset must not land under `shared/`.** 111 KB of generated bitmap there misrepresents
  what `shared/` is for.

---

## 10. What this plan does not do

- No BG22 renderer adoption — that is C-B6 and may be declined.
- **No deferral of split-panel transport.** C-B3 implements it for ESP32 and Nordic because the
  renderer contract cannot truthfully advertise two consumers without it.
- **No dual-CS support on BG22.** This is target policy, not deferred work. Canonical config fields
  and backend primitives do not weaken `OD_CAP_DUAL_CS=0`; changing it requires an explicit policy
  reversal and a new reviewed plan. B5 enforces the current policy at panel selection.
- No `od_hal_panel` promotion or change — explicitly out of scope (§ 2.2).
- No unrecorded content or visual change. B4 may change BG22 key-display policy; B2/B3 alter
  pixels; B1 adds the Nordic logo; and C-B6 adds the shared layout elements on BG22. Each is
  deliberate and independently pinned.
- No new wire surface. The boot screen has none — the landing URL is its only external contract,
  and C-B2 pins it rather than altering it.
