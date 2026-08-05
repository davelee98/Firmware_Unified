# CLAUDE.md

Guidance for Claude Code working in **Firmware_Unified**. The workspace-level
`../CLAUDE.md` describes the whole OpenDisplay multi-repo layout and the end-to-end
pipeline; this file covers only this repo.

## What this repo is

One git repository for **all OpenDisplay firmware targets**, replacing four independently
versioned repos (`Firmware`, `Firmware_NRF54`, `Firmware_Silabs`, `Firmware_NRF`) that each
reimplemented the same wire protocol, config parsing, transfer state machines, compression,
and session encryption.

**Status: one target imported and running.** `targets/esp32-idf/` builds **10 boards** and has
been flashed and exercised on an ESP32-S3 (2026-08-03). This file said "scaffold — nothing to
build" until 2026-08-05; that is no longer true and the guidance below replaces it.

- **`./build.sh` in `targets/esp32-idf/` builds everything.** It sources ESP-IDF itself (the
  install is never on `PATH`). `tools/run_host_tests.sh` runs the host tests;
  `compat/ratchet.sh` and `tools/sdkconfig_baseline.sh` are the two gates a change must not
  break. The other two targets — `nordic-zephyr`, `efr32bg22-slc` — are still **README-only**,
  so "compile all targets" means those 10 ESP32 boards.
- **`shared/` is still empty, and that is still by design.** The first promotion
  (`od_config.c`) has not happened — see docs/NEXT_STEPS.md item 5. So the ESP32 target holds
  logic that is destined for `shared/core`, and touching it means reading MIGRATION.md first.
- **The HALs are real now.** `targets/esp32-idf/hal/` implements od_hal_{nvs,log,gpio,time,
  i2c,adc,panel} against docs/SHARED_API_DESIGN.md. That document has been corrected ten times
  by contact with the implementations — trust the headers over the design doc where they
  disagree, and fix the doc.
- Do not treat the absence of a file as a bug to fix. `shared/core/`, `shared/hal/`,
  `shared/compress/` and `tools/` still hold placeholder READMEs on purpose.

**The Arduino shim is nearly gone.** `compat/` is down to 5 counted files from 21, and **5 is
the floor**, not a stall: those files are counted only for `TARGET_NRF` arms that do not
compile on this target and leave with migration step 4. Do not "finish" them here — see
`targets/esp32-idf/compat/SHIM_BUDGET`, which records every step and why.

**`targets/esp32-idf/vendor/fastepd/` is NOT a shim and does not die with `compat/`.** It is the
permanent FastEPD vendor adapter (an Arduino `SPI` object over IDF's `spi_master`), kept in its
own directory precisely so that "delete `compat/` at the floor" stays unambiguous. It is off the
component include path and granted per-source in `main/CMakeLists.txt`; so are both vendored
panel libraries. Adding a consumer is a deliberate edit there, not an `#include`.

## The one rule

`shared/` compiles for **every** target, so it may use only the C standard library and
`shared/hal` interfaces. **No vendor or framework header** — `esp_*`, `driver/*`, `soc/*`,
`hal/*`, `freertos/*`, `nrf_*`, `nrfx`, `sl_*`, `em_*`, `zephyr/*`, `Arduino.h`,
`bluefruit.h`, `NimBLE*`, `bb_epaper`, `TFT_eSPI`. If a file needs one, it belongs in
`targets/<target>/`.

This is the invariant that makes the repo worth having; the moment one vendor include slips
into `shared/`, that file stops being shareable and the repo quietly becomes four codebases
in one directory. It is enforced mechanically by
[.github/workflows/shared-boundary.yml](.github/workflows/shared-boundary.yml) — run that
grep locally before proposing anything under `shared/`. Extend the pattern list as new
targets are imported.

`shared/` is **plain C, not C++.** Two targets are C-only, so shared interfaces are
link-time-bound `extern` C functions the target implements — never C++ virtual classes, and no
function-pointer vtables except the one deliberate `od_panel_ops` exception (a single target
legitimately has 2-3 panel backends); see docs/SHARED_API_DESIGN.md, consequence 1. **That
exception is now used**: `targets/esp32-idf/hal/od_hal_panel.h` defines `struct od_panel_ops`
with bb_epaper and FastEPD backends selected at runtime. It is the only vtable in the tree and
must stay that way.
Arduino's `String` must never appear.

`third_party/` (top level, outside `shared/`) is the exception by design: `bb_epaper` picks its
IO backend by `#ifdef` and each backend includes vendor headers, so it cannot satisfy the rule
— but it is still one vendored copy for all targets. Do not "fix" this by moving it into
`shared/`.

## Layout

```
shared/          target-agnostic; no vendor SDK headers
  protocol/      canonical wire contract (synced copy — see below)
  core/          command dispatch, config parsing, transfer state machines, session auth
  compress/      inflate engines (uzlib bit-serial, ROM tinfl adapter) behind one API
  hal/           abstract interfaces targets implement (time, gpio, spi, i2c, nvs, radio,
                 crypto, log, panel)
targets/         one dir per target: chip drivers + build system + HAL implementation
  esp32-idf/            ESP32-S3/C3/C6/classic, ESP-IDF         (from Firmware)
  nordic-zephyr/        nRF54L15 + nRF52840, west/NCS           (from Firmware_NRF54, Firmware)
  efr32bg22-slc/        EFR32BG22, Simplicity SDK + CMake       (from Firmware_Silabs)
third_party/     vendored cross-target libs with per-target IO (bb_epaper)
tools/           protocol-header sync, provisioning, device CLI
docs/            architecture, toolchain, and migration notes
```

There is deliberately **no `targets/nrf52-sdk/`**. The legacy nRF52 (bare Nordic SDK) is *not*
migrated here — but it **is shipped**, so it is maintained in place in `Firmware_NRF` and it
sets the host's backward-compatibility floor (no compression, no `0x76`, no PIPE, no NFC).
Do not re-create the directory as a placeholder: a directory under `targets/` means "a target
this repo builds." See [docs/MIGRATION.md](docs/MIGRATION.md) § "Order and rationale" item 5.

## Toolchains — no PlatformIO, no Arduino

Targets are grouped **by silicon vendor, not by repo of origin**. Three build systems, all
CMake:

- **ESP-IDF** for every ESP32 variant. Pin one explicit release; floors are ≥ 5.1 (C6) and
  ≥ 5.2 (`driver/i2c_master.h` — do not use the deprecated `driver/i2c.h`).
- **Zephyr / nRF Connect SDK** for both Nordic parts. nRF52840 is a *board* on this target, not
  a target of its own — it shares the BT host, PSA Crypto, NVS, and panel stack with nRF54L15.
- **Simplicity SDK + SLC** for EFR32BG22, **unchanged**. Rejected moving it to Zephyr on RAM:
  32 KB total, already fully accounted for, and the firmware has no kernel at all (superloop +
  `sl_power_manager`).

The source `Firmware` repo builds with PlatformIO + Arduino; **both** of its halves change
toolchain on the way in. Do not carry `platformio.ini`, `lib_deps`, Arduino APIs, or
`build_flags` idioms into this repo.

**Do not design `shared/`'s configuration surface around Kconfig.** ESP-IDF and Zephyr have it;
the Silabs target does not. `shared/` takes plain preprocessor constants, and Kconfig is only how
two of the three targets set them.

[docs/TOOLCHAINS.md](docs/TOOLCHAINS.md) has the full analysis: the Arduino-API replacement
census with call-site counts, the PlatformIO-knob → sdkconfig translation table, and what is
already portable (`bb_epaper` is already an ESP-IDF component upstream; Seeed_GFX's ESP32
processor layer is already IDF-level). Read it before touching a build file.

**All three toolchains are installed on the primary dev box** (verified 2026-07-25) — but
**none is on `PATH`**, so `which idf.py west` returns nothing and it is easy to conclude they
are absent. An earlier version of this file did exactly that. Each needs activation:

| Toolchain | Version | Activate with |
|---|---|---|
| ESP-IDF | v5.5.4 | `source ~/esp/esp-idf/export.sh` |
| nRF Connect SDK / west | v3.3.1 / west v1.5.0 | `nrfutil toolchain-manager launch --ncs-version v3.3.1 -- <cmd>` |
| Simplicity SDK | 2025.12.2 | `slt`; `slc` additionally needs the bundled Java on `PATH` |

**ESP-IDF is the only one of the three that has actually built anything here** — 10 boards, and
an S3 that has run. For west and Simplicity SDK only version strings have been run, because
`targets/nordic-zephyr/` and `targets/efr32bg22-slc/` contain nothing to build yet. **Do not
claim a build passes without having run it.** See docs/TOOLCHAINS.md for the full table and the
version pins these imply.

## Protocol header — do not hand-edit

`shared/protocol/opendisplay_protocol.h` and `opendisplay_structs.h` are byte-for-byte copies
of the canonical source in `../opendisplay-protocol`. They are pure `#define` constants and
config structs — no functions, no per-target types (CI rejects function declarations here).

Change the canonical file, then propagate:

```bash
cd ../opendisplay-protocol
tools/sync_protocol_header.py --push     # canonical -> all copies
tools/sync_protocol_header.py --check    # fail if any copy drifted
```

**Known gap:** the sync tool's copy map does not include this repo yet (it lists only the
four original repos), so the headers here are an unpoliced one-time copy and `--check` will
not catch drift. Adding `Firmware_Unified/shared/protocol/` as a destination is an early
to-do — flag it rather than working around it.

## Migration constraints

Full plan and rationale in [docs/MIGRATION.md](docs/MIGRATION.md); the boundary spec is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Read both before proposing structural changes.
The rules that most often get violated by well-meaning edits:

- **One target at a time**, in the documented order: `esp32-idf` → `nordic-zephyr` (nRF54L15
  boards) → `efr32bg22-slc` → `nordic-zephyr` (nRF52840 board). ESP32 is first because it is the
  most complete implementation and becomes the reference for what `shared/core` exposes; Silabs
  is third rather than last because its 32 KB / no-kernel / no-Kconfig constraints are what keep
  `shared/` honest, and they must bite before other targets bake in assumptions it cannot meet.
- **Import sources unchanged in one commit**, then fix only include paths and build files. Two
  documented exceptions: the ESP32 import changes framework (three phases, incl. a temporary
  `arduino_compat.h` shim), and the Silabs import must not carry in its 57 MB vendored SDK
  verbatim. Both are specified in MIGRATION.md — read it before importing anything.
  Keep blame meaningful; record the source repo and commit SHA in the import commit message.
- **Do not batch subsystem swaps.** Replace a target's copy with the shared implementation
  one subsystem at a time (config parsing, then dispatch, then each transfer path), with a
  build + flash + hardware verify between each. Each swap must be independently revertable.
- **Nothing is deleted from the original repos** until the unified target is verified on
  hardware.
- **Divergence is resolved deliberately, and written down.** The four repos do not implement
  identical semantics today. When promoting logic to `shared/`, do not let the behaviour be
  settled by whichever repo happened to be copied first.

## Non-goals

- **One build system.** Three toolchains stay three — ESP-IDF, west/Zephyr, and SLC each own
  their target directory. Unification is about shared *source*.
- **Lowest-common-denominator features.** Targets legitimately differ (PSRAM, ROM inflate, panel
  families, and whether the PIPE sliding-window path exists at all — the Silabs target does not
  implement it). Express capability differences via config and `#if`, not by removing features;
  a target that lacks a feature must not pay for it in flash or RAM.
- **Rewriting working drivers.** Import them as-is; only shared logic is refactored.

## Memory sensitivity

RAM varies enormously: EFR32BG22 32 KB, nRF52840 256 KB, ESP32-S3 512 KB + PSRAM. Code in
`shared/` must avoid large static buffers sized for the biggest target, unbounded heap
allocation, and assuming a heap exists at all. Buffer sizes belong behind compile-time
constants the target sets, with a documented floor.

**`MAX_CONFIG_SIZE` is the exception: 4096 on every target** (decided 2026-07-25 — a global cap,
not a per-target macro; BG22 was raised from 2048). It is the one size deliberately *not*
target-parameterised, because a uniform value removes a wire divergence a host could not
discover. The BG22 pays for it — see docs/MEMORY_CONSTRAINTS.md item 3 for the required
mitigations and the NVM3 checks that gate the Silabs swap.
