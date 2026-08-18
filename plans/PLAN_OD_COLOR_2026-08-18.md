# OD Color Promotion Plan

**Status:** Implemented on `feat/od-color-promotion`; software-qualified, hardware pending  
**Date:** 2026-08-18  
**Scope:** Promote direct-write color-format classification and byte geometry into `shared/core/od_color.c` and `shared/core/od_color.h`.

> **Owner decision — GRAYSCALE_8:** Reserve canonical enum value 9 as a valid placeholder identity
> only. This plan must not define or implement wire bytes for it. In particular, it must not add a
> packed 3-bpp stream, encode eight levels in 4-bpp nibbles, expand 3-bpp input into a FastEPD
> framebuffer, select an Inkplate waveform, or render it through a monochrome fallback. Every
> operational path continues to reject it explicitly. Enabling eight-gray display behavior requires
> a separate, device-qualified protocol/backend plan.

This plan supersedes the color/geometry portion of `PLAN_TRANSFER_PROMOTION_2026-08-17.md` Phase 1 and `DEDUP_6_SWEEP_2026-08-14.md` section 2. It does not promote boot-screen rendering policy, panel-driver policy, or dual-chip-select support.

## 1. Objective

Create one authoritative, target-independent definition of the direct-write stream layout for every supported e-paper color scheme. ESP32, Nordic, and EFR32BG22 must use that definition to determine:

- stream layout;
- row padding;
- number and meaning of stream parts;
- bytes in each part and in the complete transfer;
- initial controller plane; and
- whether the scheme is eligible for the monochrome partial-write protocol.

The result must remove both target copies of `opendisplay_display_color.c/.h` and the equivalent direct-write arithmetic on ESP32. No target may silently reinterpret an unknown scheme as monochrome.

For this repository-local implementation, “valid placeholder” means firmware code has the stable
name and numeric identity `OD_COLOR_SCHEME_GRAY8 = 9` for classification and explicit rejection.
It does **not** alter the vendored wire header, make value 9 an advertised capability, define an
image-transfer format, or make it an admissible operational display configuration.

## 2. Ground truth

For implemented schemes, the wire contract is the encoder behavior in `py-opendisplay`, not the current target helpers. `GRAYSCALE_8` is assigned the canonical placeholder value 9, but this plan deliberately defines no direct-stream encoding for it. The promoted implementation must reproduce these layouts:

| Scheme | Direct stream layout | Row geometry | Parts | Initial plane | Partial eligible |
|---|---|---:|---:|---:|---:|
| `MONO` | packed rows | 1 bpp | 1 | 0 | yes |
| `BWR` | concatenated controller planes | 1 bpp per plane | 2 | 0 | no |
| `BWY` | concatenated controller planes | 1 bpp per plane | 2 | 0 | no |
| `BWRY` | packed rows | 2 bpp | 1 | 1 | no |
| `BWGBRY` | packed rows | 4 bpp | 1 | 1 | no |
| `GRAY4` | concatenated controller planes | 1 bpp per plane | 2 | 0 | no |
| `GRAY16` | packed rows | 4 bpp | 1 | 0 | no |
| `SEVEN_COLOR` | packed rows | 4 bpp | 1 | 1 | no |
| `BWGBRY_SPLIT` | left half followed by right half | 4 bpp per half | 2 | none; target-owned split path | no |

All packed rows are padded independently to a whole byte. For width `w`, height `h`, and part depth `b`:

```text
row_bytes(w, b) = ceil(w * b / 8)
part_bytes       = row_bytes * h
```

For `BWR`, `BWY`, and `GRAY4`, both parts have width `w` and are independently row-padded 1-bpp planes.

`GRAYSCALE_8` is a legitimate eight-level grayscale classification. Reserve canonical value 9 as
`OD_COLOR_SCHEME_GRAY8`; do not reuse value 7, which remains `SEVEN_COLOR`. The reservation is an
enum/API placeholder only. It does not define a bit depth, packing order, row geometry, controller
format, or upload behavior. `od_color_direct_geometry()` must return `OD_COLOR_UNSUPPORTED` for
value 9, and host encoders must continue to reject attempts to upload it. A future implementation
requires a separate protocol and backend plan based on a qualified device, because the Inkplate 10
example in `epaper-dithering` does not establish an OpenDisplay wire format.

For `BWGBRY_SPLIT`, the host splits the image before packing. The parts have widths `floor(w / 2)` and `ceil(w / 2)` and each half is independently row-padded at 4 bpp. Consequently, split geometry is not interchangeable with an ordinary 4-bpp frame. For example, a 122-pixel row occupies 31 bytes per half, or 62 bytes total, rather than 61 bytes.

The following values have no supported e-paper direct-stream geometry and must return `OD_COLOR_UNSUPPORTED`, not 1-bpp geometry:

- `GRAYSCALE_8` (reserved placeholder value 9);
- `RGB565`;
- `RGB888`;
- `RGB16BPC`; and
- any unknown numeric value.

Zero width or height is invalid. Arithmetic must be checked before narrowing to `uint32_t`, even though current panel dimensions fit comfortably.

### 2.1 GRAYSCALE_8 is a firmware-local compatibility placeholder

Sibling repositories under `/home/davelee/opendisplay/` are read-only references. This branch must
not edit, branch, generate into, commit, or push `opendisplay-protocol`, `py-opendisplay`,
`epaper-dithering`, or any other sibling. That durable workspace rule is also recorded in
`AGENTS.md`.

The frozen vendored header `shared/protocol/opendisplay_structs.h` therefore remains byte-for-byte
unchanged. `shared/core/od_color.h` supplies the compatibility macro
`OD_COLOR_SCHEME_GRAY8 ((uint8_t)9u)` as a local fallback. This gives local code a collision-free
identity to reject without claiming that the wire contract has changed. The C preprocessor cannot
detect enum members, so when a canonical enum reservation is eventually synced, the fallback must
be removed explicitly while retaining numeric value 9 and the unsupported behavior.

The sibling repositories establish two reference facts only: host upload currently rejects eight
gray, and palette/index support does not define device bytes. No sibling change is part of this
implementation. A future coordinated protocol reservation is an external follow-up and must retain
the same prohibitions: no 3-bpp packing, no 4-bpp substitute, and no inferred FastEPD/Inkplate path.

## 3. Defects corrected by the promotion

This is not a behavior-neutral extraction. Centralizing the current helpers without these corrections would preserve known contradictions:

1. EFR32BG22 computes flat pixel counts instead of padding every row.
2. EFR32BG22 does not classify `GRAY4` as a two-plane stream and starts it on the wrong direct-write plane.
3. All current generic fallbacks can classify `SEVEN_COLOR` as 1 bpp although the host encodes it at 4 bpp.
4. 8-level gray has no wire representation. This plan reserves `GRAYSCALE_8 = 9` so protocol and
   library APIs use one stable identity, but deliberately leaves it unsupported by direct geometry,
   upload encoding, target admission, and boot rendering. The former value-7 implementation was
   wrong; value 7 remains `SEVEN_COLOR`. Reserving value 9 must not revive either the historical
   4-bpp behavior or invent a 3-bpp behavior.
5. The target helpers do not describe `BWGBRY_SPLIT`; its independently padded half-streams require distinct geometry.
6. Using `bits_per_pixel == 1` as the partial-write test incorrectly admits `BWR` and `BWY`. Only `MONO` is eligible for the partial protocol.
7. Unknown and non-e-paper RGB schemes silently fall through to monochrome on some paths.
8. Nordic uses literal scheme numbers in the helper rather than the canonical protocol enumeration.

Each correction needs a host test that fails against the corresponding old behavior.

## 4. Shared interface

Add the module to the PURE tier in `shared/sources.cmake`. It must have no static mutable state, heap use, logging, transport dependency, display-driver dependency, or target conditional.

Proposed public interface:

```c
typedef enum {
    OD_COLOR_OK = 0,
    OD_COLOR_UNSUPPORTED,
    OD_COLOR_BAD_GEOMETRY,
    OD_COLOR_OVERFLOW,
} od_color_status_t;

typedef enum {
    OD_COLOR_LAYOUT_PACKED_ROWS = 0,
    OD_COLOR_LAYOUT_CONTROLLER_PLANES,
    OD_COLOR_LAYOUT_SPLIT_HALVES,
} od_color_layout_t;

typedef enum {
    OD_COLOR_PLANE_0 = 0,
    OD_COLOR_PLANE_1 = 1,
    OD_COLOR_PLANE_NONE = 0xff,
} od_color_plane_t;

typedef struct {
    od_color_layout_t layout;
    uint8_t bits_per_pixel;
    uint8_t part_count;
    od_color_plane_t initial_plane;
    bool partial_supported;
    uint32_t part_width[2];
    uint32_t row_bytes[2];
    uint32_t part_bytes[2];
    uint32_t total_bytes;
} od_color_geometry_t;

od_color_status_t od_color_direct_geometry(
    uint8_t color_scheme,
    uint32_t width,
    uint32_t height,
    od_color_geometry_t *geometry);
```

Interface rules:

- `bits_per_pixel` is the packing depth within each described part, not an ambiguous aggregate depth.
- `bits_per_pixel` accepts the implemented depths 1, 2, and 4; calculations must not assume that pixels divide evenly into bytes.
- `layout` defines the meaning of two parts: controller planes versus left/right halves.
- A one-part layout sets index 1 fields to zero.
- Every call with a non-null output initializes the complete structure deterministically, including error returns; a null output returns `OD_COLOR_BAD_GEOMETRY`.
- `initial_plane` uses shared logical values 0 and 1 for ordinary streams; target adapters map them to vendor constants where necessary. Split streams use `OD_COLOR_PLANE_NONE` because their parts are halves, not controller planes.
- `partial_supported` is a protocol property and is true only for `MONO`.
- `BWGBRY_SPLIT` geometry describes the host stream but does not authorize any target to drive a dual-CS panel.
- Consumers must check the status. There is no convenience function that converts an unsupported scheme into a default geometry.

If implementation shows that a field has no consumer, remove that field before landing rather than preserving speculative API. Do not replace the descriptor with the five old boolean/integer helpers: those helpers conflate logical pixel depth, per-plane packing depth, and transfer layout.

## 5. Ownership boundaries

### `od_color` owns

- canonical direct-stream classification;
- row and part byte geometry;
- controller-plane versus split-half classification;
- normal initial-plane metadata; and
- monochrome partial-protocol eligibility.

### Targets continue to own

- whether a configured panel and driver support a scheme;
- display-controller plane constants and commands;
- dual-CS admission and routing;
- FastEPD-specific transfer formats;
- allocation and buffering;
- wire error/NACK selection; and
- boot-screen rendering policy.

### Explicit exclusions

Boot rendering is not a direct-stream decoder. In particular:

- boot `GRAY4` begins as a logical packed 2-bpp row and is split on-device;
- direct-write `GRAY4` arrives as two already-separated 1-bpp planes;
- current boot support deliberately renders `SEVEN_COLOR` as a monochrome placeholder; and
- the ESP32 FastEPD ED103 path has a panel/backend-specific 4-bpp behavior.

The initial `GRAYSCALE_8` boot policy is explicit rejection, not a monochrome rendering fallback.
The shared boot lookup and every target adapter must handle reserved value 9 safely, must not index
a scheme table by an unchecked value, and must not infer a pixel format for it.

Therefore the generic boot `bits_per_pixel` and plane-selection seams must not be replaced mechanically with `od_color_direct_geometry()`. When the Nordic and EFR32BG22 helper files are deleted, retain their existing boot behavior in clearly named, boot-local adapters. A later boot-format promotion can resolve that separate contract.

## 6. Target migration

### 6.1 Nordic Zephyr

1. Replace direct-write calls to `opendisplay_color_*` with one checked `od_color_direct_geometry()` result created at START.
2. Store only the descriptor fields needed while the transfer is active; do not recompute geometry per chunk.
3. Change partial admission from the 1-bpp proxy to `partial_supported`.
4. Preserve the exact boot-screen behavior in a boot-local adapter.
5. Delete `src/opendisplay_display_color.c/.h` and remove their build registration.
6. Preserve the existing wire response for an invalid START, but route unsupported/RGB schemes through that failure path instead of accepting them as mono.
7. Reject reserved `GRAYSCALE_8` through the checked unsupported START path; no Nordic panel/backend enables it in this plan.

### 6.2 EFR32BG22

1. Replace direct-write flat-size arithmetic with the shared row-padded descriptor.
2. Use `layout` and `part_bytes[0]` for the `BWR`, `BWY`, and `GRAY4` plane transition.
3. Keep the 256-byte display row buffer and its existing silent-overflow policy unchanged; this promotion must not add a fail-loud path.
4. Preserve the custom boot renderer with boot-local scheme mapping.
5. Delete `opendisplay_display_color.c/.h` and remove their build registration.
6. Keep the target policy that dual-CS panels are unsupported. Describing `BWGBRY_SPLIT` does not make it admissible on BG22.
7. Reject reserved `GRAYSCALE_8`; BG22 gains no eight-gray behavior from the enum reservation.

### 6.3 ESP32 IDF

1. Replace `directWriteComputeGeometry()` and `calc_controller_plane_bytes()` arithmetic with the shared descriptor.
2. Use `part_bytes[]`, not pixel-count formulas, for transitions and completion.
3. Replace the partial `getBitsPerPixel() == 1` test with `partial_supported`.
4. Retain `getBitsPerPixel()`/`getplane()` only where boot rendering or a driver-specific path still requires them. Rename or comment them so they cannot be mistaken for the direct-stream authority.
5. Wrap, rather than generalize, the FastEPD ED103 exception: a small ESP32-owned adapter may override the shared normal geometry only for the exact existing backend/panel condition. Capture its pre-promotion behavior in a target test before changing the call site.
6. Do not put `FASTEPD`, panel IDs, or target compile definitions into `od_color`.
7. Reject reserved `GRAYSCALE_8`; do not add a FastEPD/Inkplate adapter or infer one from `epaper-dithering` in this plan.

### 6.4 Protocol and host tooling boundary

No protocol or host-tooling repository is modified by this implementation. Keep the vendored
protocol header frozen, keep existing local schemas/allowlists from advertising value 9, and use
siblings only as read-only behavioral references. The firmware host suite proves value 7 remains
seven-color while local placeholder value 9 has no direct geometry or boot rendering. Canonical
reservation and host error-message changes, if desired later, are separately owned follow-ups.

## 7. Tests

### 7.1 Shared host suite

Add a table-driven `tests/host/color_test.c`, linked against the normal shared-core host library. Cover every supported and rejected scheme using canonical enumeration names.

Required widths:

```text
1, 2, 3, 4, 7, 8, 9, 121, 122, 127, 128
```

Use at least heights 1, 2, and 250. Include explicit zero-dimension and null-output tests and verify deterministic zeroed output on every error with a non-null output.

For 122 × 250, assert at minimum:

| Scheme/layout | Expected part bytes | Expected total |
|---|---:|---:|
| `MONO` | 4,000 | 4,000 |
| `BWR`/`BWY`/`GRAY4` | 4,000 + 4,000 | 8,000 |
| `BWRY` | 7,750 | 7,750 |
| ordinary 4 bpp | 15,250 | 15,250 |
| split 4 bpp | 7,750 + 7,750 | 15,500 |

Add exact assertions for layout, part count, widths, row sizes, initial plane, and partial eligibility—not only total size.
For reserved `GRAYSCALE_8`, assert `OD_COLOR_UNSUPPORTED` and a deterministically initialized output
descriptor for every tested width and height; it has no expected byte count.

### 7.2 Host-encoder oracle

Cross-check the C vectors against the pinned `py-opendisplay` encoder used by the repository gate. The oracle must compare the byte length of each encoded part, including odd widths and independently padded split halves. Keep fixed expected values in the C suite so the test remains useful when Python dependencies are unavailable. For `GRAYSCALE_8`, cross-check rejection rather than bytes: Python must refuse encoding and C must return `OD_COLOR_UNSUPPORTED`.

### 7.3 Mutation proof

Demonstrate that the suite catches at least these mutations:

- replacing row padding with flat pixel-count rounding;
- classifying `GRAY4` as one packed 2-bpp part;
- returning any geometry for reserved `GRAYSCALE_8`;
- treating `SEVEN_COLOR` as 1 bpp;
- treating `BWGBRY_SPLIT` as an ordinary packed 4-bpp frame;
- permitting partial writes for `BWR` or `BWY`; and
- returning mono geometry for an unknown or RGB scheme.

Record the mutation commands and caught-test names in the implementation report; do not leave mutation switches in production code.

### 7.4 Existing regression suites

The existing boot golden tests must remain byte-exact. This is the guard that the direct-stream promotion did not accidentally change boot rendering. Existing transfer, dispatcher, compression, and target compile tests remain mandatory.

## 8. Structural ratchets

Extend `tools/check.sh` so the final tree fails when:

- either target-local `opendisplay_display_color.c/.h` file returns;
- an `opendisplay_color_*` symbol is referenced;
- a direct-write path calls the legacy generic `getBitsPerPixel()` or `getplane()` helpers;
- a target reintroduces one of the removed direct total-byte helpers; or
- `od_color.c` is registered outside the single shared PURE source list.

Ratchets must be narrow enough not to reject legitimate boot-only or controller-specific calculations. Prefer symbol/call-site checks over broad searches for arithmetic operators.

## 9. Implementation sequence

Keep the work reviewable and each commit buildable:

1. **Scope guard** — create the firmware feature branch and record the sibling-read-only and
   Gray8 restrictions in `AGENTS.md`.
2. **Contract and shared core** — add `od_color.c/.h`, exhaustive fixed-vector/rejection tests,
   boot rejection, mutation evidence, and PURE-tier registration without editing the frozen
   protocol header.
3. **Nordic adoption** — migrate direct and partial paths, preserve boot locally, verify value-9
   rejection, delete the Nordic helper, and run all Nordic fragments.
4. **EFR32BG22 adoption** — migrate direct geometry/plane changes, preserve the 256-byte
   silent-overflow and no-dual-CS policies, verify value-9 rejection, delete the BG22 helper, and
   run the Simplicity Studio target build.
5. **ESP32 adoption** — migrate ordinary direct geometry and partial eligibility, preserve and
   test only the exact FastEPD ED103 adapter, verify value 9 remains rejected, and build all ESP32
   fragments.
6. **Ratchets and documentation** — enable final structural checks and update repository-local
   status documents to name `od_color` as the direct-stream authority and Gray8 value 9 as a
   local reserved/unsupported placeholder. Record canonical/host coordination only as a future
   external follow-up.

Temporary duplicate ownership is acceptable within the implementation branch only. It must not remain in the final commit series.

## 10. Verification and measurements

Run the complete gate, not only the new unit test:

```text
tools/check.sh --targets
```

Acceptance requires:

- GCC and Clang host suites;
- ASan and UBSan host suites;
- the Python wire/encoder corpus;
- every ESP32 build fragment;
- every Nordic build fragment;
- the EFR32BG22 target build; and
- zero structural-check exclusions added to make the promotion pass.

Capture before/after flash and static RAM for all three target families using the same toolchain and configuration. `od_color` should add no static mutable storage and no heap allocation. Any unexplained static-RAM increase is a stop condition. Report flash deltas even when link-time folding makes them zero or negative.

Also inspect the map or symbols to confirm that only one color-geometry implementation is linked per image and that `od_color` contributes no `.bss` state.

### 10.1 Implementation result

The repository-local implementation is complete on `feat/od-color-promotion`:

- the color contract is owned by `shared/core/od_color.c/.h` and registered once in the PURE
  source tier;
- the host color suite passes 3,961 checks, including exhaustive row-padding vectors and explicit
  value-9 rejection;
- the complete 38-test host suite passes;
- all 11 ESP32 build variants pass;
- all three Nordic board builds pass; and
- the EFR32BG22 target build passes.

The target-local Nordic and EFR32BG22 color helpers are removed, and structural ratchets reject
their return or a second registration of `od_color.c`. The full top-level gate's functional target
builds were completed individually after its first sandboxed run encountered environment-only
LeakSanitizer/SDK-cache restrictions. No board qualification has been performed, so the hardware
debt in section 11 remains open for all three target families.

## 11. Hardware qualification

Software gates establish the geometry contract but do not establish controller behavior. When boards are available, run a real client through:

- a width not divisible by 8, including the final row;
- `BWR` or `BWY` across the plane boundary;
- `GRAY4` across the plane boundary;
- `SEVEN_COLOR` on hardware that genuinely supports it;
- a rejected non-mono partial START; and
- the ESP32 FastEPD profile whose geometry is preserved by the adapter.

EFR32BG22 testing must also confirm that promotion does not alter the fixed 256-byte row-buffer policy. Do not add dual-CS testing to BG22; the target continues to reject that configuration by policy.

`GRAYSCALE_8` has no hardware-qualification item in this plan because it has no implementation to
qualify. An Inkplate 10 test must not be used to quietly turn the placeholder into a FastEPD 4-bpp
adapter; that work starts with a separate approved protocol/backend plan.

If hardware is unavailable, the change may report software completion, but it must retain an explicit per-target hardware-debt entry. It must not be described as hardware-qualified.

## 12. Stop conditions

Stop and revise the design before landing if any of the following occurs:

- the C geometry differs from the host encoder for any supported width;
- any host, target, or boot path accepts value 9 as an encodable/displayable format;
- a sibling repository is mutated as part of this firmware-local implementation;
- the vendored `shared/protocol/opendisplay_structs.h` is edited directly instead of syncing from
  `../opendisplay-protocol`;
- any bit depth, packing order, row geometry, or controller mapping is assigned to value 9;
- a target needs target macros or panel IDs in `od_color`;
- boot golden output changes;
- the FastEPD exception cannot be isolated without changing its existing wire contract;
- BG22 requires a larger row buffer or a new failure policy;
- describing split geometry accidentally enables dual-CS operation; or
- an invalid scheme can still reach a display driver as implicit monochrome.

## 13. Definition of done

The promotion is complete only when:

- `shared/core/od_color.c/.h` are the sole direct-stream color/geometry authority;
- the two target helper pairs and ESP32 direct geometry duplicates are gone;
- all implemented schemes match host bytes and geometry, including `SEVEN_COLOR` and odd-half split padding;
- reserved firmware-local `GRAYSCALE_8 = 9` is rejected by shared direct geometry, boot rendering,
  direct-write admission, and every target backend without changing the frozen vendored protocol
  header or any sibling repository;
- only `MONO` is accepted by the partial protocol;
- unsupported/RGB schemes fail explicitly;
- boot, FastEPD, BG22 buffer, and dual-CS policies are unchanged;
- all software gates pass with target builds;
- before/after flash and RAM measurements are recorded; and
- hardware status and any remaining debt are stated without qualification ambiguity.
