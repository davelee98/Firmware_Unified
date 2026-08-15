# CLAUDE.md

Firmware_Unified: one repo for all OpenDisplay firmware targets, replacing four repos
(`Firmware`, `Firmware_NRF54`, `Firmware_Silabs`, `Firmware_NRF`) that each reimplemented the
same wire protocol, config parsing, transfer state machines, compression and session encryption.
`../CLAUDE.md` covers the wider workspace.

**ALL SIX SOURCE REPOS ARE CHECKED OUT AS SIBLINGS AND ARE READABLE — USE THEM.** `../Firmware`,
`../Firmware_NRF54`, `../Firmware_Silabs`, `../Firmware_NRF`, plus `../opendisplay-protocol` (the
canonical wire contract) and `../py-opendisplay` (the host, and the answer to "does the client
actually do X?"). `targets/*/` here are import *snapshots* that drift; the siblings are live. Diff
before you port — see § "Migration constraints".

## Reading budget

Code first. Headers, build files and tests are ground truth; `docs/` explains *why* and is never a
prerequisite. This file already distils it — don't open a doc to re-confirm a rule stated here, or
to answer what a grep answers. Dated files (`*_REVIEW_<date>`, `FINDINGS*`, NEXT_STEPS.md) are
archive. One doc per task, at most; past that, write the code and flag the uncertainty.

## Comments

Say what the code does and why it is the way it is. Do not narrate history — no "this used to",
no "before the promotion", no recounting the bug that prompted the change. Git and the commit
message hold that. A comment longer than the code it explains is almost always history in
disguise. Keep the non-obvious constraint (a wire contract, a RAM ceiling, an ordering that
looks arbitrary and is not); delete the story.

## Status

- **Two HARDWARE-VERIFIED targets.** `targets/esp32-idf/` — 10 boards, run on an ESP32-S3. And
  `targets/nordic-zephyr/` **as of 2026-08-14, on the `xiao_nrf52840` board only**: image upload,
  config write + reload, and host-side MSD decode all exercised on a flashed device. Its other
  two boards (`xiao_nrf54l15`, `xiao_nrf54lm20a`) still build clean but have NOT been flashed, so
  the target is verified, the L15 is not. `efr32bg22-slc` builds headless
  (`./build-and-flash.sh --no-flash`) and has never been flashed.
- **THERE IS NO CI. `tools/check.sh` (repo root) is every gate this repo has, and nothing runs
  it but you.** Boundary greps, the host suite under gcc + clang, the same suite under
  ASan/UBSan, the pre-auth fuzz targets, the py-opendisplay wire corpus, and the shim ratchet;
  `--esp32` adds the board builds and the sdkconfig baseline. **A SKIP IS NOT A PASS** — missing
  clang or ESP-IDF skips rather than fails, so read the summary, which reprints skips and exits
  2 when there were any.
- Paths in this bullet are relative to `targets/esp32-idf/`. `./build.sh` there builds every board
  fragment (it sources ESP-IDF itself; never on `PATH`). `tools/run_host_tests.sh` runs host tests
  — or drive them directly, `cmake -S tests/host -B <dir> && cmake --build <dir> && ctest
  --test-dir <dir>`, which is the repo-root path and needs no ESP-IDF. `compat/ratchet.sh` and
  `tools/sdkconfig_baseline.sh` are gates a change must not break.
- **`shared/` is no longer empty** — `core/od_{adv_control,advert,config,config_asm,config_tlv,watchdog}.c`
  plus header-only `od_span.h`, all listed in `shared/sources.cmake` (never globbed) in per-HAL
  tiers. Consumers: host tests and `esp32-idf` take the aggregate; `nordic-zephyr` takes PURE +
  HAL_WDT; `efr32bg22-slc` takes PURE only — called on Nordic, still compiled-only on Silabs
  except `od_advert`.
  **`od_watchdog` is no longer a scaffold, and NOT YET HARDWARE-VERIFIED on either target.**
  Both `esp32-idf` (`hal/od_hal_wdt.c`, over the IDF Task Watchdog) and `nordic-zephyr`
  (`src/od_hal_wdt.c`, over the devicetree `watchdog0` + `gpregret2` nodes) implement
  `od_hal_wdt.h` and call the policy through a per-target `od_watchdog_app` owner. Two things
  to know before flashing: arming widens the ESP32 idle-task TWDT check from 60 s to
  `OD_WDT_TIMEOUT_S` (300 s, the ~240 s panel-refresh bound), and on Nordic only `main()`
  feeds — a wedge confined to the display work queue does not trip it.
  **CALLED AND HARDWARE-VERIFIED on `esp32-idf` (2026-08-13):** `od_adv_control`, `od_advert`,
  `od_config_asm`, `od_config_tlv`, `od_span` — a board flashed with the `od_advert` swap wrote
  a config carrying an NFC packet and completed an encrypted image upload.
  **CALLED AND HARDWARE-VERIFIED on `nordic-zephyr` (2026-08-14, `xiao_nrf52840`):** `od_advert`
  and `od_config`. `struct od_config` is the parsed-config aggregate (the `struct GlobalConfig`
  copies are gone), `od_config_parse()` is the whole of `loadGlobalConfig()`, and Nordic's
  530-line packet switch, its own CRC-16 and its own size table went with it. Evidence from one
  flashed board: image upload completes; a config write is re-parsed across a reboot; the host
  decodes the MSD correctly, with battery and temperature right (the two fields `od_advert`
  re-encodes, including its float-domain clamp) and button presses reaching the dynamic block
  (so the `binary_inputs` packet survives the new parse and lands where the encoder reads it).
  That upload only passed once BLE TX power was fixed — see `zephyr/CMakeLists.txt`
  `OD_TX_POWER_DBM`, which also records why Zephyr's `BT_CTLR_TX_PWR_*` Kconfig is inert under
  the SoftDevice Controller.
  **CALLED, NOT YET FLASHED:** `od_advert` on `efr32bg22-slc` — with that, no open-coded MSD copy
  is left on any target, and `tests/host/advert_test.c` holds the two encoders they shipped as the
  differential reference (do not "update" those to match the encoder).
  `efr32bg22-slc` still open-codes the config parse: measured 2026-08-14 as
  +1 byte of RAM **only with `OD_CONFIG_WITH_{TOUCH,BUZZER,WIFI,DATA_EXTENDED}=0`** (gated 909 B
  vs its current 844 + 64; ungated 1617 B against 484 B of static slack at 98.5% RAM). Its real
  gate is `MAX_CONFIG_SIZE` 4096 + the NVM3 object-size check, not the aggregate. Most remaining
  protocol logic (dispatch, transfer, session) still lives in the ESP32 target.
- `targets/esp32-idf/hal/` implements `od_hal_{nvs,log,gpio,time,i2c,adc,panel,crypto}`.
- **`shared/hal/od_hal_crypto.h` is the third shared HAL** (2026-08-15, with `od_hal_adv` and
  `od_hal_wdt`), implemented on both `esp32-idf` (mbedTLS) and `nordic-zephyr` (native
  `psa_aead_*`, which needed only `CONFIG_PSA_WANT_ALG_CCM=y` — the hand-rolled RFC 3610 both
  Nordic targets carried existed because that Kconfig was never set, not because PSA lacked CCM).
  Prepared **key slots**, not a key in the caller's struct: the targets clear a session with
  `memset`, which would drop a live PSA handle and exhaust a finite pool. Four-valued status so a
  tag mismatch and an engine fault stay distinguishable — the session's 3-strike policy depends on
  it. **NOT YET HARDWARE-VERIFIED**, and that commit also deletes Nordic's soft CCM (preserved as
  `tests/host/session_ccm_reference.inc`), so treat the CCM path as unproven until a board
  authenticates and completes an encrypted upload.
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
Enforced by the three `shared boundary:` checks in [tools/check.sh](tools/check.sh) — run them
before proposing anything under `shared/`; extend the pattern as targets are imported.

## Architectural decisions

1. **`shared/` is plain C.** Every interface across the boundary is a link-time `extern` C
   function the target implements. No C++ classes, no Arduino `String`.
   Re-argued 2026-08-13 and upheld — but not because C++ is unavailable (all three toolchains
   already compile target `.cpp`). It is that the boundary must be a C API for Zephyr's C
   drivers and the Silabs superloop regardless, and the code being ported is already C in
   `.cpp` files (~20k lines of `targets/esp32-idf/src/` hold 32 `new`, 9 `String`, one class).
   So C++ buys implementation ergonomics only, priced in a doubled host gate, an
   `-fno-exceptions -fno-rtti -fno-threadsafe-statics` contract spread across three build
   systems `shared/` does not own, and an enforcement grep that libstdc++'s transitive
   includes defeat. Three C-side rules buy back most of what RAII and strong types offered:
   - **`_Static_assert` every wire size.** The `sizeof`-derived size table can go silently
     wrong via include order (`shared/sources.cmake`, closing note) — a wrong *value*, not a
     build error. Assert the sizes so it is a build error. Zero cost, C99, unused today.
   - **Non-owning views are `od_span_t`** (`shared/core/od_span.h`), not ptr+len arguments.
     `od_span_split()` is the checked cut and the one place bounds arithmetic is written;
     `take`/`drop` saturate and are only for lengths already known to fit. The whole config
     parse path takes spans; new shared code does too.
   - **Single-exit + `goto cleanup`** in anything holding a resource across a fallible step.
   Revisit only at `od_session.c` / `od_xfer_partial.c` / `od_zlib_stream.c`: nested resource
   lifetimes with several failure exits per function are the one shape where manual cleanup
   reliably loses. That is also the last point where switching is cheap.
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
  **`Firmware` is the authority over `Firmware_NRF54`** when their algorithms disagree: it is the
  field-proven original and what the host tooling was validated against, and the NRF54 port
  re-derived several of them. So port the `esp32-idf` behaviour and make the Zephyr difference
  justify itself — in a differential test, the Firmware form is the reference. `esp32-idf` is
  C++ and `shared/` is plain C, so this usually means a C port, not a file move. A default, not
  a licence to skip the write-up.
- **THE AUTHORITY IS `../Firmware/`, THE SIBLING REPO — NOT `targets/esp32-idf/src/`.** That
  directory is a *snapshot* taken at import, and upstream keeps moving. Before porting or
  transcribing any algorithm, diff the two; when this repo and upstream disagree, upstream wins
  unless the difference is a deliberate Firmware_Unified adaptation (Arduino removal, `od_hal_*`,
  `od_log`, `struct od_config`), which the import is full of — so separate *drift* from
  *adaptation* rather than blanket-copying either way. `../Firmware/tools/` also carries host
  tests worth porting with the code they cover.
  Learned the hard way on `od_session` (2026-08-15): the replay window had been extracted upstream
  into `src/nonce_window.h` with an 816-line host test, and three rounds of design were built on
  the stale ring instead — two of them wrong in security-relevant ways (a forward window cap that
  strands a session, and counting nonce failures as integrity strikes so packet loss tears one
  down). A 30-second diff would have caught all of it.
  The same applies to the other three source repos for their targets.

## Memory sensitivity

EFR32BG22 32 KB, nRF52840 256 KB, ESP32-S3 512 KB + PSRAM. `shared/` must avoid buffers sized for
the biggest target, unbounded heap, and assuming a heap exists. Sizes go behind compile-time
constants the target sets, with a documented floor; decision 12 is the sole exception
(docs/MEMORY_CONSTRAINTS.md item 3 has the BG22 mitigations gating the Silabs swap).
