# CLAUDE.md

Firmware_Unified: one repo for all OpenDisplay firmware targets, replacing four repos
(`Firmware`, `Firmware_NRF54`, `Firmware_Silabs`, `Firmware_NRF`) that each reimplemented the
same wire protocol, config parsing, transfer state machines, compression and session encryption.
`../CLAUDE.md` covers the wider workspace.

## Reading budget

Code first. Headers, build files and tests are ground truth; `docs/` explains *why* and is never a
prerequisite. This file already distils it — don't open a doc to re-confirm a rule stated here, or
to answer what a grep answers. Dated files (`*_REVIEW_<date>`, `FINDINGS*`, NEXT_STEPS.md) are
archive. One doc per task, at most; past that, write the code and flag the uncertainty.

## Status

- **`targets/esp32-idf/` is the only target that builds** — 10 boards, run on an ESP32-S3.
  `nordic-zephyr` and `efr32bg22-slc` are README-only.
- `./build.sh` there builds everything (it sources ESP-IDF itself; never on `PATH`).
  `tools/run_host_tests.sh` runs host tests. `compat/ratchet.sh` and
  `tools/sdkconfig_baseline.sh` are gates a change must not break.
- **`shared/` is empty by design**; the ESP32 target holds the logic destined for it. Placeholder
  READMEs under `shared/` and `tools/` are intentional — a missing file is not a bug.
- `targets/esp32-idf/hal/` implements `od_hal_{nvs,log,gpio,time,i2c,adc,panel}`.
- **Never hardware-verified:** the WiFi/LAN transport, and the F4/F7 correctness fixes.
- **`compat/` (Arduino shim) is at its floor of 5 files** — `TARGET_NRF` arms that leave with
  migration step 4. Do not "finish" them (`targets/esp32-idf/compat/SHIM_BUDGET`).
- **`targets/esp32-idf/vendor/fastepd/` is not a shim** and outlives `compat/`: the permanent
  FastEPD adapter (Arduino `SPI` over IDF `spi_master`). It and both vendored panel libraries sit
  off the include path, granted per-source in `main/CMakeLists.txt` — adding a consumer is an edit
  there, not an `#include`.
- Live plan: docs/NEXT_STEPS_2026-08-05.md (docs/NEXT_STEPS.md is historical).

## The one rule

`shared/` compiles for **every** target: C standard library and `shared/hal` only. **No vendor or
framework header** — `esp_*`, `driver/*`, `soc/*`, `hal/*`, `freertos/*`, `nrf_*`, `nrfx`, `sl_*`,
`em_*`, `zephyr/*`, `Arduino.h`, `bluefruit.h`, `NimBLE*`, `bb_epaper`, `TFT_eSPI`. A file needing
one belongs in `targets/<target>/`. One slip and the repo is four codebases in a directory.
Enforced by [.github/workflows/shared-boundary.yml](.github/workflows/shared-boundary.yml) — run
that grep before proposing anything under `shared/`; extend it as targets are imported.

## Architectural decisions

1. **`shared/` is plain C.** Two targets are C-only: interfaces are link-time `extern` C
   functions the target implements. No C++ classes, no Arduino `String`.
2. **One vtable, deliberately** — `od_panel_ops` (`targets/esp32-idf/hal/od_hal_panel.h`), for the
   one target with 2-3 panel backends. Keep it the only one.
3. **Three toolchains stay three** (ESP-IDF, west/Zephyr, SLC), all CMake. Unification is about
   shared *source*.
4. **Grouped by silicon vendor, not repo of origin.** nRF52840 is a *board* on `nordic-zephyr`,
   sharing its BT host, PSA Crypto, NVS and panel stack.
5. **No PlatformIO, no Arduino** — no `platformio.ini`, `lib_deps`, Arduino APIs or `build_flags`
   idioms, from any source repo.
6. **BG22 stays on Simplicity SDK.** Zephyr rejected on RAM: 32 KB total, no kernel at all
   (superloop + `sl_power_manager`).
7. **No Kconfig in `shared/`'s config surface** — Silabs lacks it. Plain preprocessor constants;
   Kconfig is only how two targets set them.
8. **No `targets/nrf52-sdk/`.** Legacy nRF52 stays in `Firmware_NRF` and sets the host's compat
   floor (no compression, no `0x76`, no PIPE, no NFC). A dir under `targets/` means "a target this
   repo builds".
9. **No lowest-common-denominator features.** Differences (PSRAM, ROM inflate, panel families,
   PIPE — absent on Silabs) go behind config and `#if`; a target must not pay for a feature it
   lacks.
10. **Import working drivers as-is**; only shared logic is refactored.
11. **First `shared/` source: `shared/core/od_adv_control.c`** (portable BLE advertising/
    lifecycle). `od_config.c` is the first *protocol* subsystem promoted.
12. **`MAX_CONFIG_SIZE` is 4096 everywhere** — a global cap, not a per-target macro; a uniform
    value removes a wire divergence a host could not discover. BG22 pays for it.
13. **`third_party/` is exempt from the one rule** — `bb_epaper` picks its IO backend by `#ifdef`.
    Still one vendored copy for all targets; do not move it into `shared/`.
14. **Headers beat design docs.** Where `targets/esp32-idf/hal/*.h` and docs/SHARED_API_DESIGN.md
    disagree, fix the doc.

## Layout

`shared/{protocol,core,compress,hal}` — wire contract / dispatch + config + transfer + auth /
inflate engines / interfaces targets implement.
`targets/{esp32-idf,nordic-zephyr,efr32bg22-slc}` — chip drivers + build system + HAL impl.
`third_party/` vendored cross-target libs (bb_epaper); `tools/`; `docs/`.

## Toolchains

Installed, but **none on `PATH`** — `which idf.py west` returning nothing ≠ absent.

| Toolchain | Version | Activate with |
|---|---|---|
| ESP-IDF | v5.5.4 | `source ~/esp/esp-idf/export.sh` |
| nRF Connect SDK / west | v3.3.1 / west v1.5.0 | `nrfutil toolchain-manager launch --ncs-version v3.3.1 -- <cmd>` |
| Simplicity SDK | 2025.12.2 | `slt`; `slc` also needs the bundled Java on `PATH` |

Pin one ESP-IDF release; floors ≥ 5.1 (C6), ≥ 5.2 (`driver/i2c_master.h` — not the deprecated
`driver/i2c.h`). Only ESP-IDF has built anything here. **Do not claim a build passes without
running it.** docs/TOOLCHAINS.md has the version pins and the Arduino-API replacement census.

## Protocol header — do not hand-edit

`shared/protocol/opendisplay_protocol.h` and `opendisplay_structs.h` are byte-for-byte copies of
`../opendisplay-protocol`. Edit the canonical file there, never these.

## Migration constraints

Rationale in [docs/MIGRATION.md](docs/MIGRATION.md) / [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
— open only if a change contradicts a rule here or adds a target.

- **One target at a time:** `esp32-idf` → `nordic-zephyr` (nRF54L15) → `efr32bg22-slc` →
  `nordic-zephyr` (nRF52840). ESP32 first as the reference for `shared/core`; Silabs third so its
  32 KB / no-kernel / no-Kconfig limits bite before other targets bake in assumptions.
- **Import unchanged in one commit**, then fix only include paths and build files; record source
  repo + SHA. Exceptions: the ESP32 framework change, and Silabs' 57 MB vendored SDK.
- **One subsystem per swap**, build + flash + hardware verify between each, independently
  revertable.
- **Delete nothing from the original repos** until the unified target is hardware-verified.
- **Resolve divergence deliberately and write it down** — not by whichever repo was copied first.

## Memory sensitivity

EFR32BG22 32 KB, nRF52840 256 KB, ESP32-S3 512 KB + PSRAM. `shared/` must avoid buffers sized for
the biggest target, unbounded heap, and assuming a heap exists. Sizes go behind compile-time
constants the target sets, with a documented floor; decision 12 is the sole exception
(docs/MEMORY_CONSTRAINTS.md item 3 has the BG22 mitigations gating the Silabs swap).
