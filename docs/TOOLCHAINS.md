# Toolchain analysis

Which build system and framework each target uses in `Firmware_Unified`.
Scope: **ESP32 (all `platformio.ini` variants), nRF52840, nRF54L15, EFR32BG22.**
`Firmware_NRF` (legacy nRF52 + bare Nordic SDK) is out of scope — it **is** still shipped
(a handful of low-capability units, established 2026-07-25), but it is maintained in place in
its own repo and never migrates here; see MIGRATION.md § "Order and rationale" item 5.

## Decision

**Three toolchains, split by silicon vendor. No PlatformIO and no Arduino anywhere in the repo.**

| Target | Chips | Toolchain | Build | Change from today |
|---|---|---|---|---|
| `targets/esp32-idf/` | ESP32-S3 (N8R8/N16R8/N32R8, ext-UART, E1004), ESP32-C3 (N4/N16), ESP32-C6 (N4), classic ESP32 (N4) | **ESP-IDF** + `idf.py` | CMake + Kconfig | **drops PlatformIO and Arduino** |
| `targets/nordic-zephyr/` | nRF54L15, nRF52840 | Zephyr / nRF Connect SDK + west | CMake + Kconfig | **nRF52840 moves off PlatformIO/Arduino, joins nRF54L15** |
| `targets/efr32bg22-slc/` | EFR32BG22 (`…C222F352GM40`) | **Simplicity SDK** + SLC | CMake (generated) | **none — keep as-is** |

The two Nordic/Espressif targets keep nothing from the current PlatformIO setup; both end up
on **CMake + Kconfig**, which is the property that makes this more than a preference — see
below. The Silabs target is the exception in every dimension: it is already CMake, it is
already vendor-SDK-native, and it is the one target where changing anything is a net loss
(§ EFR32BG22).

## Why this is the right shape (not just a preference)

**`shared/` becomes one CMake library consumed by every target.** This is the strongest
argument for ESP-IDF and it is architectural, not stylistic. Under PlatformIO, `shared/` would
have had to be consumed by PIO's SCons-based library scanner *and* by Zephyr's CMake — two
dependency models, two sets of include-path rules, two ways for a vendor header to sneak in.
With IDF, `shared/` is an `idf_component_register()` component on one side and a
`zephyr_library()`/`target_sources(app ...)` on the other, from the same source list.

**Buffer sizes become Kconfig options, which is what ARCHITECTURE.md already asked for.** Its
memory-sensitivity section requires that buffer sizes be "target-parameterised behind
compile-time constants the target sets, with a documented floor." IDF and Zephyr both have
Kconfig. That turns `-DOPENDISPLAY_ZLIB_WINDOW_BITS=15`, `-DOPENDISPLAY_ZLIB_USE_HEAP_WINDOW`,
and `-DPIPE_SMALL_DRAM_WINDOW` from ad-hoc `build_flags` strings into declared options with
types, defaults, ranges, and help text — in the same idiom on those two targets.

**But do not design `shared/`'s configuration surface around Kconfig**, because the Silabs
target has none (see § EFR32BG22). `shared/` takes plain preprocessor constants; Kconfig is
merely the mechanism by which two of the three targets set them.

**The platform-fork problem disappears entirely.** `Firmware/platformio.ini` currently depends
on *three* platform sources: `pioarduino/platform-espressif32` (all S3 envs + classic ESP32),
the official `espressif32` registry (C3-N4, C6-N4, C3-N16), and
`maxgerhardt/platform-nordicnrf52` (nRF52840) — two of them community forks. Today the C3/C6
envs and the S3 envs can build against different Arduino cores and different IDF versions
**from the same source tree**, which is a live source of "works on S3, fails on C6" divergence
that has nothing to do with unification. Going to IDF + west removes all three dependencies in
favour of vendor-supported SDKs.

**Eliminating `String` is work that has to happen anyway.** `String` appears at **575 sites**
across `Firmware/src/` — by far the largest Arduino dependency. `shared/` must be plain C
(two targets are C-only), so every `String` on a path destined for `shared/` was going to be
removed regardless. The Arduino removal and the `shared/` extraction are substantially the
same work, done once instead of twice.

## Evidence from the current tree

Findings that drove the analysis, worth re-checking if they change.

### bb_epaper is already an ESP-IDF component upstream

This was the risk I expected to dominate, and it is already solved. The upstream checkout at
`~/bb_epaper` has:

- a top-level `CMakeLists.txt` that is literally
  `idf_component_register(SRCS ... REQUIRES driver esp_timer)`;
- `esp_idf/esp_generic.inl` — an IDF IO backend implementing `digitalWrite`/`pinMode`/SPI over
  `driver/gpio.h`, `driver/spi_master.h`, `esp_timer.h`;
- `esp_idf/esp_main_io.inl`, plus `s3_ulp_io.inl` and `c6_ulp_io.inl` (ULP-coprocessor
  backends);
- working IDF example projects, including a Spectra6 one.

Backend selection happens by `#ifdef` inside `bb_epaper.cpp`, which already dispatches to
`esp_idf/esp_generic.inl` as its non-Arduino, non-Zephyr, non-Silabs default.

**Caveat — no copy is a superset.** `Firmware_NRF54/third_party/bb_epaper/` and
`Firmware_Silabs/third_party/bb_epaper/` have no `esp_idf/` directory; upstream `~/bb_epaper`
(at `2ef09a1`) has no `nrf54_zephyr_io.inl`, `nrf54_bbep_busy.inl`, `silabs_efr32_io.inl`, or
`silabs_bbep_busy.inl`. The single vendored copy has to be assembled from both — see "Where
bb_epaper and uzlib live" below.

**Correction to a plausible assumption:** `esphome_io.inl` is *not* a usable IDF backend
despite ESPHome's IDF association — it includes `<SPI.h>` and calls `pinMode`/`HIGH`, i.e. it
targets ESPHome's Arduino path. The genuine non-Arduino backends are `esp_idf/esp_generic.inl`,
`nrf54_zephyr_io.inl`, and `silabs_efr32_io.inl` — the last two existing only downstream, in the
`Firmware_NRF54` and `Firmware_Silabs` vendored copies.

### Seeed_GFX / TFT_eSPI is a bounded port, not a rewrite

`Firmware/lib/Seeed_GFX/` is vendored in-tree (so it can be modified freely) and its ESP32
processor layer is **already IDF-level**:

```
Processors/TFT_eSPI_ESP32.h      → soc/spi_reg.h, driver/spi_master.h, hal/gpio_ll.h
Processors/TFT_eSPI_ESP32_S3.h   → same, plus OpenDisplay/opendisplay_runtime_pins.h
```

There is already an OpenDisplay-specific hook (`OpenDisplay/opendisplay_runtime_pins.h`), so
the fork is established. What remains Arduino: ~87 call sites in `TFT_eSPI.cpp`
(`Serial`/`String`/`digitalWrite`/`pinMode`/`delay`/`millis`/`SPI.`) and `FS.h`/`SPIFFS.h` for
font loading, which is very likely unused here and can be `#if 0`'d.

Only two S3 envs enable it (`-DOPENDISPLAY_SEEED_GFX`, for `panel_ic_type` 3000/3001), so it is
also isolatable — it does not block the other eight boards.

### The ESP32 application code is already IDF-level

`wifi_service.cpp` includes `esp_wifi.h`, `esp_event.h`, `esp_heap_caps.h`, and raw
`mbedtls/ssl.h`, `mbedtls/ctr_drbg.h`, `mbedtls/entropy.h` alongside `WiFi.h`/`ESPmDNS.h`.
Across `src/`, direct IDF calls (`esp_sleep_*`, `esp_deep_sleep_*`, `esp_restart`,
`esp_wifi_*`, `esp_event_*`) are routine. Arduino is a veneer over code that is already
IDF-shaped.

### Arduino API census — what actually has to be replaced

Call sites across `Firmware/src/` (`*.cpp` + `*.h`), with the IDF and HAL destination:

| Arduino API | Sites | ESP-IDF replacement | HAL destination |
|---|---:|---|---|
| `String` | **575** | `std::string` / fixed `char[]` | must not appear in `shared/` at all |
| `Wire` | 246 | `driver/i2c_master.h` (IDF ≥ 5.2) | `od_hal_i2c` |
| `pinMode` | 102 | `gpio_config()` | `od_hal_gpio` |
| `digitalWrite` | 87 | `gpio_set_level()` | `od_hal_gpio` |
| `delay()` | 77 | `vTaskDelay()` | `od_hal_time` |
| `millis()` | 66 | `esp_timer_get_time() / 1000` | `od_hal_time` |
| `LittleFS` | 23 | `esp_littlefs` component — or NVS, see below | `od_hal_nvs` |
| `delayMicroseconds` | 20 | `esp_rom_delay_us()` | `od_hal_time` |
| `digitalRead` | 13 | `gpio_get_level()` | `od_hal_gpio` |
| `SPI.` | 6 | `driver/spi_master.h` | `od_hal_spi` |
| `attachInterrupt` | 5 | `gpio_isr_handler_add()` | `od_hal_gpio` |
| `Serial.` | **5** | `esp_log` / USB-Serial-JTAG | `od_hal_log` |
| `analogRead` | 1 | `adc_oneshot` | — (fold into battery driver) |

Two things to read out of this table. **`Serial.` is only 5 sites** because logging already
goes through `writeSerial(String message, bool newLine)` in `main.h` — the abstraction exists,
it just has the wrong signature; it becomes `od_hal_log`. And **`String` is 28% of `Wire`+
everything else combined**, concentrated in `config_parser.cpp` (160), `display_service.cpp`
(98), and `wifi_service.cpp` (41) — the first two being exactly the files whose logic is
destined for `shared/core`.

### The nRF54L15 port is a full second implementation of the shared logic

Near-duplicate subsystems, written twice in two languages:

| Subsystem | `Firmware` (Arduino/C++) | `Firmware_NRF54` (Zephyr/C) |
|---|---|---|
| Config parsing | `config_parser.cpp` (919) | `opendisplay_config_parser.c` (696) |
| Transfers / PIPE | `communication.cpp` (754) + part of `display_service.cpp` (3309) | `opendisplay_pipe.c` (1425) |
| BLE | `ble_init.cpp` (321) + `esp32_ble_callbacks.h` (131) | `opendisplay_ble.c` (756) |
| Crypto | `encryption.cpp` (848), 2 `#ifdef` backends | PSA (`CONFIG_PSA_WANT_*`) |
| Config storage | Adafruit_LittleFS / LittleFS | `opendisplay_config_storage.c` (NVS/settings) |

The Zephyr side is already **C**, for everything except `opendisplay_display.cpp` and
`boot_screen.cpp`. Since `shared/` must be C, the nRF54L15 sources are the better donor for
*structure and language*, while `Firmware` remains the reference for *feature coverage*.

### `Firmware_NRF54` already has a nascent HAL

`nrf54_zephyr_compat.h` is four functions — `od_msleep`, `od_uptime_get_32`, `od_busy_wait`,
`od_hwinfo_get_device_id` — plus `nrf54_gpio.c` and `nrf54_zephyr_time.c`. That is `od_hal_time`
and `od_hal_gpio` in embryo, already using the `od_` prefix. Promote these rather than
designing from scratch.

### Only the Silabs toolchain is installed on this dev box

`~/.platformio/platforms/` has `espressif32` and `nordicnrf52`. There is **no `~/ncs`, no
`west`, and no `idf.py`**. So neither *recommended* toolchain is currently installed:
`Firmware_NRF54` cannot be built here today, and IDF will need installing before any ESP32
work starts.

The EFR32BG22 toolchain, by contrast, **is** installed and verified building headless
(2026-07-20, Simplicity SDK 2025.12.2):

| Piece | Location |
|---|---|
| ARM GCC 12.2.Rel1 | `~/.silabs/slt/installs/conan/p/gcc-a442105b5c2637/p/bin/` (also the hardcoded fallback in `cmake_gcc/toolchain.cmake`) |
| `slc-cli` 6.0.17 | `~/.silabs/slt/installs/archive/slc-cli-v6.0.17/slc_cli/slc` |
| Simplicity Commander | `~/.silabs/slt/installs/archive/commander/commander` (`.gbl` OTA + J-Link flash) |
| OpenJDK 21 (for `slc`) | `~/.silabs/slt/installs/archive/java21-v21.0.5/java21_linux_x86_64/jre/bin` — **not on `PATH`** |
| Simplicity SDK 2025.12.2 | `~/.silabs/slt/installs/conan/p/simpl965e19baece23/p` — full SDK, 2892 `.slcc`, 2.4 GB (`slt where simplicity-sdk`) |
| `cmake`, `ninja`, `slt` | system-wide / `~/bin/slt` |

The full Simplicity SDK **is installed here** (verified 2026-07-25) — `2025.12.2`, the exact
version the build uses, at `~/.silabs/slt/installs/conan/p/simpl965e19baece23/p`: 2892 `.slcc`
files, 2.4 GB, a real SDK rather than a `-cp` source copy. So the pinned-install requirement
(MIGRATION.md § "The Silabs SDK is not imported as-is") is already met on this machine.

**The remaining gap is registration, not installation.** No `.slconf` exists in the project and
`~/.uc/cli/cli.config` registers no SDK, so `slc generate` still fails unless pointed at the
install with `--sdk-package-path`. Earlier notes — including the workspace-level `CLAUDE.md` —
record this as "no full SDK available"; that is now out of date and should be corrected to "SDK
present, not wired to `slc`". Independently of either, the CMake build needs neither `slc` nor
the SDK metadata, so this remains the one target that builds on this machine today.

Consequences: each target needs a "what must be installed" section, and **CI matters more than
usual** because no single dev box builds everything.

## Per-target detail

### ESP32 (all variants) → ESP-IDF

**Version.** Pin one explicit IDF release in a checked-in file and use it everywhere
(`.idf_version` or a `tools/idf-env.sh` wrapper). Floors: **≥ 5.1** for ESP32-C6 support,
**≥ 5.2** for the new `driver/i2c_master.h` API that the 246 `Wire` sites should land on
(the legacy `driver/i2c.h` is deprecated — do not port onto it). Confirm what the current
stable release is at import time rather than trusting this doc.

**Board matrix.** The ten PlatformIO envs are the real ergonomic loss — PIO's `extends =
env:...` inheritance (already used by `esp32-s3-E1004`) has no direct IDF equivalent. Replace
with layered sdkconfig fragments, which IDF supports natively:

```
targets/esp32-idf/
  sdkconfig.defaults              # common to all boards
  sdkconfig.defaults.esp32s3      # per-IDF_TARGET, auto-selected by IDF
  sdkconfig.defaults.esp32c3
  sdkconfig.defaults.esp32c6
  sdkconfig.defaults.esp32
  boards/
    s3-n16r8.conf   s3-n32r8-extuart.conf   s3-e1004.conf
    c3-n4.conf      c3-n16.conf   c6-n4.conf   esp32-n4.conf   ...
  partitions/
    default_8MB.csv  default_16MB.csv  default_32MB.csv  huge_app.csv
  build.sh                        # build.sh <board>  — mirrors the Zephyr target's build.sh
```

`build.sh <board>` composes `SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/<board>.conf"` and
sets `IDF_TARGET`. That deliberately mirrors `Firmware_NRF54/build.sh` (and the Silabs target's
`build-and-flash.sh`), so all three targets are
driven the same way — worth the small effort for the symmetry alone.

**Settings translation.** Every current PIO knob has a native equivalent, most of them more
precise:

| PlatformIO | ESP-IDF sdkconfig |
|---|---|
| `board_build.partitions = default_16MB.csv` | `CONFIG_PARTITION_TABLE_CUSTOM` + `..._FILENAME` |
| `board_upload.flash_size = 16MB` | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` |
| `board_build.flash_mode = qio` / `dio` | `CONFIG_ESPTOOLPY_FLASHMODE_QIO` / `_DIO` |
| `board_build.arduino.memory_type = qio_opi`, `psram_type = qspi_opi` | `CONFIG_SPIRAM_MODE_OCT` + `CONFIG_SPIRAM_SPEED_*` |
| `-DBOARD_HAS_PSRAM` | `CONFIG_SPIRAM=y` |
| `ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1` | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` |
| `-DOPENDISPLAY_LOG_UART{,_RX,_TX}` (ext-UART boards) | `CONFIG_ESP_CONSOLE_UART_CUSTOM` + TX/RX pin options |
| `-DCONFIG_FREERTOS_WATCHDOG_TIMEOUT_S=120` | `CONFIG_ESP_TASK_WDT_TIMEOUT_S=120` (it was always an IDF option, passed as a raw `-D`) |
| `board_build.filesystem = littlefs` | `esp_littlefs` managed component — or drop, see below |
| `-DPIPE_SMALL_DRAM_WINDOW` (classic ESP32 only) | project Kconfig option, default `y` for `esp32` |
| `-DOPENDISPLAY_ZLIB_WINDOW_BITS`, `..._USE_HEAP_WINDOW` | project Kconfig options with ranges + defaults |
| `extra_scripts = pre:scripts/factory_config_gen.py` | CMake `add_custom_command` — copy the pattern from `Firmware_NRF54/zephyr/CMakeLists.txt`, which already does exactly this |

**The one genuinely new piece: BLE.** IDF ships NimBLE natively, but only the **C host API**
(`ble_gap_*`, `ble_gatts_*`). NimBLE-Arduino's C++ wrapper — which `ble_init.h` currently
aliases wholesale (`using BLEDevice = NimBLEDevice;` and nine more) — does not exist in IDF. So
`ble_init.cpp` + `esp32_ble_callbacks.h` (~450 lines) get rewritten against the C API. Two
mitigations: `opendisplay_ble.c` (Zephyr, C, 756 lines) is a structural reference for
"OpenDisplay GATT service written against a C host API", and this is precisely the code that
`od_hal_radio` is supposed to abstract — so the rewrite is the HAL implementation, not throwaway
work.

**Config storage — converge on NVS.** The 23 `LittleFS` sites are all in `config_parser.cpp` /
`main.h`, i.e. config blob persistence and nothing else. The Zephyr side already does this with
NVS (`opendisplay_config_storage.c`). Recommend **NVS on both** rather than adding the
`esp_littlefs` managed component, which collapses `od_hal_nvs` to one semantic on both targets.
Verify first that LittleFS holds nothing else on ESP32 (fonts? boot images?) — if it does, keep
`esp_littlefs` for that and still put config in NVS.

**What is straightforwardly better under IDF:** `menuconfig` instead of `build_flags` strings,
native partition/OTA tooling, `esp_littlefs`/NVS as declared components, no community platform
fork in the dependency chain, and direct access to the IDF APIs the code already calls without
Arduino sitting in between.

### nRF52840 → Zephyr / nRF Connect SDK

The case for this is **stronger** once Arduino leaves the ESP32 side: keeping nRF52840 on
PlatformIO/Arduino would leave exactly one Arduino toolchain, for one chip, wrapping a
third-party fork (`maxgerhardt/platform-nordicnrf52`) of a platform wrapping the Adafruit nRF52
core wrapping the deprecated SoftDevice model.

**Why it fits.** The nRF52840 env's own `lib_ignore = Seeed_GFX, NimBLE-Arduino` says its scope
is *bb_epaper + BLE + config + transfers* — precisely the nRF54L15's scope, already implemented
in Zephyr. Moving it:

- leaves Zephyr's BT host as the single Nordic BLE stack (no Bluefruit);
- replaces `Adafruit_LittleFS`/`InternalFileSystem` and `Adafruit_nRFCrypto` +
  `nrf_cc310/crys_aesccm.h` (raw CC310 headers, visible in `encryption.cpp`) with Zephyr
  NVS/`settings` + PSA Crypto — the same APIs nRF54L15 uses;
- deletes an `#ifdef` arm from `encryption.cpp` (the other, mbedTLS + `esp_random`, becomes the
  IDF path);
- **makes nRF52840 a board, not a target.** `board_nrf54.c` already switches on
  `NRF54_BOARD_LM20` vs `NRF54_BOARD_L15`, and `zephyr/CMakeLists.txt` already selects conf
  files via `BOARD MATCHES`. A third Nordic board extends an existing pattern.

**What it costs.** A board port, but a real one: devicetree/overlay and pinctrl for the custom
nRF52840 board (replacing `variants/nrf52840custom`), a `prj.conf` for the nRF52 series,
re-verifying deep sleep and the `NRF_ADV_BOOST_*` advertising-interval logic against Zephyr's
BT API, replacing the UF2 packaging step (`scripts/nrf_uf2_post.py`) with MCUboot/`west sign`
or equivalent, and moving factory provisioning to the CMake path. **Flash-layout and DFU
compatibility with already-deployed nRF52840 units is the item most likely to bite** and needs
settling before the port starts.

**Verify** (I could not check these — no NCS on this box): that the pinned NCS version still
supports the nRF52 series (it should; nRF52 remains supported in NCS v3.x — the deprecated one
is the old nRF5 SDK), that PSA Crypto on nRF52840 provides AES-CCM and CMAC via `nrf_cc3xx`,
and that nRF52840 + Zephyr BT + the 512-byte MTU settings in `prj.conf` leave enough RAM
alongside the panel and inflate buffers.

### nRF54L15 → Zephyr / nRF Connect SDK

**No decision to make.** nRF54L is supported only by nRF Connect SDK — no Arduino core, no
PlatformIO platform. Keep `west` plus the existing `build.sh`/`flash.sh`/`ncs-env.sh` wrapper,
which already handles profile selection (`battery` | `uart`), board-conditional conf files,
factory-config generation, and NCS auto-detection.

Two things to fix rather than carry forward: **pin the NCS version** (`ncs-env.sh` globs
`~/ncs/v3.*` then `~/ncs/v2.*` and takes the first hit — fine for one dev, not reproducible
across machines or CI), and keep the RTT-vs-UART profile split, since the battery build has no
USB UART.

### EFR32BG22 → stay on Simplicity SDK + SLC

**Keep it. This is the one target where the current toolchain is already the right answer**,
and the only one that adds a third build system to the repo. Full target detail — file
inventory, budgets, build invocation, divergences — is in
[../targets/efr32bg22-slc/README.md](../targets/efr32bg22-slc/README.md); this section
covers only the toolchain choice.

The alternative worth naming is folding it into `targets/nordic-zephyr/` as a Silabs board —
Zephyr does support Series 2 SoCs, and recent Zephyr carries an HCI driver that wraps the
Silabs link-layer library. **Reject it, on RAM.** The chip has 32 KB, and the shipping
build already ends with static RAM at `__HeapBase = 0x200056b0` (~22 KB) and a 10.3 KB
heap — the entire 32 KB, accounted for. Zephyr's kernel plus the BT host does not fit in
what is left, and the current firmware is not merely bare-metal by preference: it has **no
kernel component at all** (`app_bm.c`, `sl_power_manager`, a superloop). Porting would mean
re-earning the memory budget from scratch to gain nothing the build system currently costs
us. Flash is nearly as tight: 236.8 KB of a 272 KB app region, ~87 % full.

What this costs the repo, stated plainly:

- **A third build system**, against ARCHITECTURE.md's "three toolchains remain three
  toolchains" non-goal — which this satisfies, but it is worth being honest that the
  vendor-split rationale that justifies two here justifies three.
- **`shared/` gets no Kconfig on this target.** The ESP-IDF/Zephyr argument that buffer
  sizes become declared Kconfig options with types and ranges does not extend to SLC.
  Here they are `target_compile_definitions` in `cmake_gcc/CMakeLists.txt`
  (`OD_ENABLE_RTT`, `OD_APP_VERSION`, …) and generated `-D`s. So `shared/` must keep taking
  its sizing from **plain preprocessor constants**, with Kconfig on the other two targets
  merely being how those constants get set. Do not design the `shared/` configuration
  surface around Kconfig.
- **A pinned SDK install is a hard prerequisite** — `slc generate` needs a full Simplicity SDK
  with `.slcc`/`.slce` metadata, which the source-only `-cp` tree does not carry. The unified
  repo therefore **requires `slt install simplicity-sdk` at the pinned version** and does not
  vendor the SDK (MIGRATION.md § "The Silabs SDK is not imported as-is"). This is the same
  class of prerequisite as IDF and NCS below, and the same answer applies: CI has to build it.

What the repo gets in return, and it is more than it looks:

- **`opendisplay_config_parser.c` (529 lines) already has zero vendor includes** — the only
  application file across the four repos that satisfies the `shared/` rule today, and the
  smallest of the three config parsers (`Firmware` 919 C++, `Firmware_NRF54` 696 C).
- **It is the constraint that keeps `shared/` honest.** Bare-metal, no threads, no blocking
  sleep, 32 KB, no Kconfig. Any `shared/core` API that assumes a scheduler, a heap it can
  lean on, or a 32 KB window buffer is excluded by this target before it ships — which is
  exactly what MIGRATION.md wants from putting Silabs third rather than last.

**The zlib window divergence is the concrete instance of that**, and it contradicts a note
carried at the workspace level. This target pins `OPENDISPLAY_ZLIB_WINDOW_BITS=9` — a
**512-byte** window, not the 32 KB that "existing targets pin 32 KB windows for legacy-client
compatibility" implies. 32 KB is a third of the chip's RAM; it can never exist here. So
window size is a genuine per-target parameter in `shared/compress`, 512 B is its documented
floor, and the encoder side (`py-opendisplay`) must know which devices are 9-bit. Resolve
this in writing before `shared/compress` is designed, not after.

**Verify at import time** (I could not check these here): that the panel set actually
overlaps — `opendisplay_epd_map.c` maps 66 panel ids to `bb_epaper` constants and whether
that is a subset or a superset of the other targets' maps decides how much of the map is
shareable; and that the PSA Crypto usage in `opendisplay_pipe.c` (AES-CCM + CMAC via
`psa_mac_compute`/`psa_import_key`) is API-identical to nRF54L15's, which would make
`od_hal_crypto` one implementation for two targets.

**Two mechanical traps** for whoever wires `shared/` into this target. First, adding `shared/`
and `third_party/` sources must go through the `.slcp`, never by hand-editing the generated
`cmake_gcc/opendisplay-bg22.cmake` — the next regen overwrites it. With the SDK installed at a
pinned version this stops being theoretical: regen goes from ~never to routine, so a hand edit
that skips the `.slcp` will be silently deleted rather than surviving indefinitely. Second, the
bundled OpenJDK 21 is not on `PATH`, so bare `slc` reports
`java: command not found`; that reads as a missing runtime but is only a PATH miss.
`bb_epaper` already has a `silabs_efr32_io.inl` backend, so the panel stack needs no new work.

## Where bb_epaper and uzlib live

Both are cross-target but they are **not** the same kind of dependency:

- **`third_party/bb_epaper/`** (top level, *outside* `shared/`). Its IO backends are selected by
  `#ifdef` inside `bb_epaper.cpp` and each includes vendor headers — so the file transitively
  references `driver/gpio.h`, `zephyr/*`, or `sl_*` depending on target. It can never satisfy
  the `shared/` rule and must not live there. Today `Firmware` pulls it from git via `lib_deps`
  while `Firmware_NRF54` and `Firmware_Silabs` each vendor a copy — one vendored copy resolves
  that split.

  **Correction, now that Silabs is in scope: there is no superset copy to vendor.** Upstream
  `~/bb_epaper` has `esp_idf/` but **no** `nrf54_zephyr_io.inl`, `nrf54_bbep_busy.inl`,
  `silabs_efr32_io.inl`, or `silabs_bbep_busy.inl`. Those four backends exist only in the
  `Firmware_NRF54` and `Firmware_Silabs` vendored copies, which in turn lack `esp_idf/`.
  Neither side is complete, so the vendored copy has to be **assembled**: upstream as the base
  plus the four downstream backend `.inl`s and their `#ifdef` arms in `bb_epaper.cpp`. Worth
  upstreaming them so this stays a checkout rather than a fork — check first whether upstream
  has since taken them.
- **`shared/compress/uzlib/`**. Pure C, no vendor headers, already vendored identically in both
  repos (`lib/uzlib` and `third_party/uzlib`). This one genuinely belongs under `shared/`.

The CI boundary check in `.github/workflows/shared-boundary.yml` greps `shared/` only, so
`third_party/` is already outside its scope — but add a comment there recording *why*
`third_party/bb_epaper` is exempt, or someone will eventually "fix" it by moving it in.

## Consequences for the scaffold — applied

The scaffold has been updated to match this analysis. Recorded here so the reasoning stays
attached to the change:

1. **`targets/` layout** — `esp32-nrf52840-pio/` → `esp32-idf/`, `nrf54l15-zephyr/` →
   `nordic-zephyr/` (both now multi-board), `nrf52-sdk/` marked out-of-scope/EOL. *(Superseded
   2026-07-25: `targets/nrf52-sdk/` is now deleted outright, not marked — the legacy fleet is
   shipped and stays in `Firmware_NRF`. See MIGRATION.md § "Order and rationale" item 5.)*
2. **`README.md`** — target table rebuilt around the vendor split; `third_party/` added to the
   layout; "no PlatformIO, no Arduino" stated up front.
3. **`docs/ARCHITECTURE.md`** — the "one build system" non-goal reworded (this lands on *three*
   build systems chosen per vendor, not one); `shared/` documented as plain C with no `String`;
   the forbidden-header list extended with the IDF families (`driver/*`, `soc/*`, `hal/*`,
   `freertos/*`); the memory-sensitivity section pointed at the existing HAL embryos.
4. **`docs/MIGRATION.md`** — order changed to **ESP32 (IDF) → nRF54L15 (Zephyr) → Silabs →
   nRF52840 as a board on the Zephyr target**, with `nrf52-sdk` dropped. Silabs stays third
   rather than last for the reason given in § EFR32BG22: it is the constraint that keeps
   `shared/` honest, so it must bite before three targets bake in assumptions it cannot meet.
   nRF52840 goes last because it is the only step that is pure toolchain migration with no new
   capability — the one to defer if priorities shift. Also added: the three-phase ESP32 import
   (unchanged → `arduino_compat.h` shim → shim demolition) added, because "import unchanged, fix
   only build files" does not survive a framework change; concurrent-development-on-`Firmware`
   promoted to the top risk.
5. **`CLAUDE.md`** — toolchain section added; `third_party/` exemption explained so it does not
   get "fixed" into `shared/`.

One ARCHITECTURE.md non-goal is worth re-reading as confirmation rather than contradiction:
"rewriting working drivers" still holds — the ESP32 drivers keep their logic and change only
their IO calls, and the nRF52840 port reuses the nRF54L15 drivers.

## Risks

- **This lands the largest single piece of work on the most active codebase.** `Firmware` is
  under active development; a multi-week IDF port will accumulate brutal rebase conflicts
  against concurrent feature work. Either freeze feature work on `Firmware` for the duration,
  or accept that the port will be re-done against a moving target. Decide deliberately, up
  front — this is the risk most likely to derail the migration, and it is a scheduling problem,
  not a technical one. *(Decided 2026-07-25: frozen for the duration of the port —
  MIGRATION.md § "Risks to watch".)*
- **`String` removal touches 575 sites**, concentrated in the files whose logic is going to
  `shared/` anyway. High volume, low per-site difficulty, but it is where the hours go.
- **BLE is a genuine rewrite** (~450 lines, NimBLE C API). Everything else is mechanical or
  already IDF-level.
- **This dev box can build exactly one of the three targets** (EFR32BG22). Install IDF and NCS,
  and stand up a CI matrix (per-board `idf.py build` + a west build + the BG22 CMake build)
  early. The `shared/` boundary grep is necessary and nowhere near sufficient.
- **EFR32BG22 has no headroom for `shared/` to be careless.** ~35 KB of 272 KB flash free and a
  32 KB RAM budget already fully accounted for (~22 KB static + 10.3 KB heap). A shared
  implementation that costs a few KB more than the target's hand-written one is a build failure
  here, not a regression to tune later — so measure `.bss`/`.text` on BG22 after every subsystem
  swap, not at the end.
- **Seeed_GFX is only used by two S3 boards.** If the port stalls, ship the other eight and come
  back to it — do not let it block.

## Open questions

- **Is nRF52840 still in the product line, or is nRF54L15 its replacement?** Decides whether
  the Zephyr port is worth doing at all. *Answered 2026-07-25: shipped, yes — MIGRATION.md
  § "Deployed fleet status"; the port stays step 4.*
- **Are there deployed nRF52840 units that must accept OTA from the current UF2/Bluefruit flash
  layout?** If so, this constrains or blocks the Zephyr move. *Largely answered 2026-07-25:
  deployed units run the Adafruit UF2 bootloader with a working BLE DFU path, and moving them
  to Zephyr almost certainly means replacing the bootloader — physical access — see
  targets/nordic-zephyr/README.md § "Deployed nRF52840". Still open is the product call
  recorded there: migrate deployed units at all, or leave them on Arduino/Bluefruit while new
  production ships Zephyr + MCUboot.*
- **Does ESP32 LittleFS hold anything besides the config blob?** Decides whether
  `esp_littlefs` is needed at all or whether NVS covers it. *Largely mooted 2026-07-25 by the
  flash-and-reconfigure decision (MIGRATION.md § "Deployed fleet status"): stored data is not
  preserved across the transition, so nothing needs migrating out of LittleFS either way. What
  survives of the question is only whether the new IDF build wants a filesystem component at
  all — answer that at import.*
- **Is `Firmware_NRF` (legacy nRF52, bare Nordic SDK) still shipped?** If not, delete it from
  the plan rather than carrying it as a fourth target. *Answered 2026-07-25: yes — a handful of
  low-capability units, maintained in place in `Firmware_NRF` and never migrated here; they set
  `py-opendisplay`'s backward-compatibility floor. MIGRATION.md § "Order and rationale"
  item 5.*
- **Can `Firmware` feature work pause during the IDF port?** See the first risk. *Decided
  2026-07-25: frozen for the duration of the port — MIGRATION.md § "Risks to watch".*
- **What zlib window does the host encoder target per device?** Settled on the firmware side:
  the default is **9 bits / 512 B on every target** (`uzlib.h:22`), targets *reject* streams
  declaring a larger window, and only `esp32-s3-E1004` builds `=15`. The "32 KB for
  legacy-client compatibility" note was simply wrong and is corrected in the workspace
  `CLAUDE.md`; `opendisplay-protocol/docs/AUDIT_2026-07-19_SUMMARY.md:113` reached the same
  conclusion independently. *The contract half has since resolved too (2026-07-25): the host
  already encodes `windowBits = 9` unconditionally and rejects its own output otherwise
  (DESIGN_REVIEW_2026-07-25.md F5), so the E1004's 15-bit build is host-unreachable dead
  capability. What remains open is narrower — whether anyone ever wants >9 bits and where the
  per-device capability value lives, which is the capability-discovery problem, not a
  compression one. `shared/compress` still exposes the window as a per-target parameter with
  512 B as the floor (MEMORY_CONSTRAINTS.md).*
- **Is `CMD_NFC_ENDPOINT` (TNB132M) going to any other target?** It is in the canonical protocol
  header but only EFR32BG22 implements it. Decides whether it belongs in `shared/core` behind a
  capability flag or stays target-local.
- **Who owns regenerating `cmake_gcc/opendisplay-bg22.cmake`?** Largely answered: the full
  Simplicity SDK 2025.12.2 **is** installed on the primary dev box, so regen is possible here
  once `slc` is pointed at it (`--sdk-package-path`, or a project `.slconf`). What is still open
  is whether that wiring is committed to the repo — until it is, `.slcp` changes keep carrying
  unverifiable hand-edits.
