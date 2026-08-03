# Target: ESP32-S3 / C3 / C6 / classic (ESP-IDF)

Source repo: `Firmware` (https://github.com/OpenDisplay/Firmware.git)

## Status: **runs on hardware** (ESP32-S3, 2026-08-03)

All ten boards build, and the S3 has been flashed and exercised. This section used to say
"Phase B in progress — configures, partially compiles, does not link" and carried a census of
six missing third-party headers; every one of those is resolved. The census is dropped rather
than updated because a list of solved problems reads as a list of open ones.

```bash
./build.sh                     # every board -> ../../release/, merged images
./build.sh s3-n16r8 c6-n4      # some
./build.sh --list --clean
```

`build.sh` sources ESP-IDF itself if it is not on `PATH` (it never is — the install is
activated per shell). Each board produces one merged image flashed at offset 0; see
[docs/BBEPAPER_IO_BACKENDS.md](../../docs/BBEPAPER_IO_BACKENDS.md) for the panel layer and
`release/MANIFEST.txt` for the per-board chip and flash command.

### Verified on hardware (S3, 2026-08-03)

Measured, not inferred — from a device log:

| | |
|---|---|
| Compressed image push | 4x 96 000 B, zlib 1.52–43.8x, 15–35 KB/s |
| Config read round-trip | `0x0040`, 805 B in 9 chunks |
| Config write round-trip | `0x0041` + `0x0042`, reloaded from storage |
| Encrypted path | `0x0050` challenge/response, session established, all traffic encrypted |
| Panel power cycle | 4 cold bring-ups, **1398 ms** each, no BUSY timeouts |
| Panel refresh | 0.71 s, full |
| Reboot (`0x000F`) | BLE deinit + controller release, clean `RTC_SW_CPU_RST` |
| Deep-sleep button wake | works |

Cold bring-up breakdown, which is worth keeping because the parts sum exactly to the total
(898 + 201 + 49 + 250 + 0 = 1398) — so there is no unaccounted time on this path:

```
[EPD cold] pwrmgm(on)      898 ms   <- 800 ms of it a hardcoded delay(800)
[EPD cold] bbepInitIO      201 ms   <- almost entirely the two delay(100) reset pulses
[EPD cold] bbepWakeUp       49 ms
[EPD cold] initSeq (full)  250 ms
[EPD cold] alignRamMode      0 ms
```

**Still unexercised**, so MIGRATION.md Gate 2 is *mostly* met rather than met — and Gate 2 is
what licenses retiring the source repo:

- an **uncompressed** full image push
- an **interrupted transfer that recovers**. One `[DROP: 1]` was observed mid-transfer (the
  transfer completed, 337/337 chunks), so this is worth testing deliberately rather than
  assumed.

### Known defects, measured on hardware

1. **FastEPD writes no pixels to an IT8951 panel.** All three
   `it8951WriteFramebuffer{1,2,4}Bit` functions have unpatched `#ifdef ARDUINO` guards (9
   sites) that fall through to nothing under IDF: no data preamble, and the row loop builds
   each line and discards it. Commands, `LD_IMG_END` and `DisplayArea` all still run, so the
   panel refreshes whatever was already in its RAM and every call reports success.
   `third_party/NOTICE.md` claims "every affected guard" was patched — that claim is wrong.
   S3-only (FastEPD is S3-only and PSRAM-mandatory), and only on IT8951/parallel panels.
2. **`bbepWaitBusy` blocks the loop task.** It polls with a bare `delay(20)` inside
   `bb_ep.inl`, so `serviceBleTx()` cannot run for the duration. A legitimate multi-second
   refresh therefore holds queued BLE responses in the TX ring until it finishes; this was the
   mechanism behind acks arriving 16 s late while the panel was faulty, and it survives the
   panel fix. `bbepLightSleep()` is the injection point for a pump.
3. **Every transfer pays a full cold bring-up** (1.4 s) because this unit has
   `Screen Timeout: 0 s`, i.e. keep-alive disabled. 800 ms of that is an unconditional
   `delay(800)` rail settle in `pwrmgm()`.
4. **`s3-e1004` still has no board fragment** — see § `s3-e1004` is blocked, not forgotten.

### Phase C, still owed

The Arduino shim is **not** gone: `compat/` is at 21 files by `compat/ratchet.sh`, and two
vendored libraries (bb_epaper via `bb_epaper.h`, FastEPD via `arduino_io.inl`) still depend on
it, so it cannot be deleted even when the imported sources stop needing it. `protocol_pending.h`
is also outstanding — two panel-IC wire values that belong in the canonical protocol header.

### Toolchain translation findings

Recorded as they are found, since TOOLCHAINS.md's translation table was written from reading
rather than building:

- **`CONFIG_FREERTOS_WATCHDOG_TIMEOUT_S=120` does not translate.** `platformio.ini` passed it
  as a `-D`, which the Arduino core accepted. Under IDF it is a real Kconfig symbol,
  `CONFIG_ESP_TASK_WDT_TIMEOUT_S`, with an enforced range of **[1, 60]** — 120 is rejected and
  silently falls back to the default. `sdkconfig.defaults` uses 60, the closest legal value.
  Whether the original 120 was load-bearing is unverified; if it was, the watchdog must be
  reconfigured at runtime instead.
- **`ARDUINO_USB_MODE` / `ARDUINO_USB_CDC_ON_BOOT` have no define equivalent.** They become
  the sdkconfig console choice (`CONFIG_ESP_CONSOLE_USB_CDC`), which is why they appear in
  `sdkconfig.defaults.esp32s3` and not in the board fragment's define list.

## Phase A (complete) — sources imported unchanged

| | |
|---|---|
| Imported from | `upstream/main` @ `2e2131b9980c9a46f1c8e56ce2d3dcb4b7aa5bd3` |
| Commit | `feat(wifi/lan): WiFi/LAN transport + tinfl inflate; pin pioarduino 55.03.39 (#124)`, 2026-07-25 |
| Contents | `src/` `lib/` `tools/` `scripts/` — 67 files, verified byte-identical to the source tree |

**This does not compile, and that is the expected state.** Phase A exists for provenance and
reviewable blame, not for a working build (../../docs/MIGRATION.md § "The ESP32 import is
different"). The sources are Arduino/PlatformIO and this target is ESP-IDF; Phase B adds the IDF
skeleton plus the temporary `compat/arduino_compat.h` shim and aims to **link and boot on
hardware early**, so every later step is bisectable against a known-good baseline.

Until Phase B lands, build this target from the original repo.

### What was deliberately NOT imported

Step 1 says "import unchanged", and these are the exceptions — decided before the first commit,
the way the Silabs SDK exclusion was, because a file committed once is carried forever:

- **`include/opendisplay_protocol.h` and `include/opendisplay_structs.h`.** They are a vendored
  copy of the canonical wire contract, and this repo already holds exactly one copy at
  `shared/protocol/`. Importing them would make this the *tenth* copy in a workspace whose
  header-sync mechanism is already measured at 1-in-sync/5-drifted/2-missing. Phase B points the
  include path at `shared/protocol/` instead. (Checked at import: the source copy differs from
  canonical by ten lines, all in a doc-only changelog entry, with no wire difference — see
  "Protocol header state" below.)
- **`platformio.ini`, `boards/`, `variants/`, `bin/`.** PlatformIO and Arduino board artifacts
  with no meaning under IDF; carrying them in would contradict the repo's "no PlatformIO, no
  Arduino" rule and leave dead files that look authoritative. The board *information* is still
  needed — it is the input to the sdkconfig translation table in ../../docs/TOOLCHAINS.md — and
  its source of truth remains the `Firmware` repo, which is not retired until migration step 4.
- The source repo's own `README.md`, `CLAUDE.md`, `AGENTS.md`, `docs/`, `LICENSE`.

### Two things to know before reading this code

**`src/` is an ESP32 *and* nRF52840 tree.** The two chip families are interleaved *within*
files — `ble_init.cpp`, `communication.cpp`, `device_control.cpp` and `display_service.cpp` all
carry Bluefruit/nRF52 code behind `#ifdef` — not separated into per-chip files. So this
directory legitimately contains Nordic code that does not belong to this target. It leaves at
migration **step 4**, when the nRF52840 half becomes a board on `targets/nordic-zephyr/`; that
is why `Firmware` is not retired until step 4 completes.

**`lib/uzlib/src/od_zlib_stream.c` is destined for `shared/compress/`, not this target.** It is
the resumable inflate engine, pure C with no vendor headers, already vendored identically in the
NRF54 and Silabs repos — SHARED_API_DESIGN.md § `shared/compress` says to lift it unchanged. It
was imported *here* rather than straight into `shared/` on purpose: promotion is migration step
3-4, and doing it during Phase A would conflate an import with a refactor and cost the clean
blame this phase exists to preserve. It is the obvious first promotion candidate.

### Protocol header state at import

The risk recorded in MIGRATION.md § "Risks to watch" — that the import would land a header stale
at protocol 2.1, missing all of SECTION 9 (LAN) — **did not materialise, and the underlying
claim was itself stale.** `upstream/main` is at 2.2 *because* `#124` is the LAN feature. The
source copy differs from canonical only by a ten-line doc-only changelog entry describing the
`0x43` trailing patch byte. Nothing needs re-syncing; DIVERGENCE_MATRIX §8.2 should be corrected.

First target in the migration order; see ../../docs/MIGRATION.md.

**This target changes framework on import.** The source repo builds with PlatformIO + Arduino;
this target is **ESP-IDF, no Arduino**. The rationale, the per-API replacement census, and the
PlatformIO-knob → sdkconfig translation table are in ../../docs/TOOLCHAINS.md. The import is
therefore a three-phase sequence (unchanged import → temporary `compat/arduino_compat.h` shim →
shim demolition subsystem by subsystem), not the plain "import unchanged" used for other
targets — see the ESP32 section of ../../docs/MIGRATION.md.

## Boards

Eleven variants in the source repo's `platformio.ini`. **Ten have fragments and all ten
build** (measured 2026-07-26, ESP-IDF v5.5.4, clean tree each time). The eleventh,
`s3-e1004`, is deliberately absent — see below.

| Board | Chip | Flash / PSRAM | FastEPD | Image | Slot free | Notes |
|---|---|---|---|---|---|---|
| `s3-n16r8` | ESP32-S3 | 16 MB + 8 MB OPI | yes | 1,260,992 | 81% | reference board |
| `s3-n8r8` | ESP32-S3 | 8 MB + 8 MB OPI | yes | 1,260,992 | 62% | |
| `s3-n32r8` | ESP32-S3 | 32 MB + 8 MB OPI | yes | 1,260,992 | 81% | |
| `s3-n16r8-extuart` | ESP32-S3 | 16 MB + 8 MB OPI | yes | 1,284,256 | 80% | console on CH343P UART (GPIO43/44) |
| `s3-n16r8-extuart-debug` | ESP32-S3 | 16 MB + 8 MB OPI | yes | 1,296,224 | 80% | `OD_LOG_LEVEL=DEBUG`; +12 KB. Debug build, not a shipping one |
| `s3-n32r8-extuart` | ESP32-S3 | 32 MB + 8 MB OPI | **no** | 1,269,248 | 81% | reTerminal Sticky; panel via bb_epaper |
| `c3-n4` | ESP32-C3 | 4 MB, no PSRAM | no | 1,327,536 | 32% | |
| `c3-n16` | ESP32-C3 | 16 MB, no PSRAM | no | 1,327,536 | 80% | DIO flash to free GPIO12/13 |
| `c6-n4` | ESP32-C6 | 4 MB, no PSRAM | no | 1,518,032 | 23% | IDF ≥ 5.1. **Shipped.** `min_spiffs_4MB.csv` — see below |
| `esp32-n4` | classic ESP32 | 4 MB, no PSRAM | no | **695,104** | 65% | no WiFi; reduced PIPE reorder window |
| `s3-e1004` | ESP32-S3 | 32 MB + 8 MB | no | — | — | **NO FRAGMENT.** Blocked: wrong bb_epaper fork vendored — see below |

Two things the table makes obvious that were not obvious before:

* **The classic ESP32 is by far the smallest image at 695 KB — 45% smaller than the S3.**
  It is the one board that does not compile the WiFi/LAN transport. NEXT_STEPS.md item 3
  expected this board to be the troublesome one; the reduced PIPE window
  (`PIPE_SMALL_DRAM_WINDOW`) was already in `structs.h` from the import and it built first try.
* **The C6 is the tightest fit by a wide margin** — 23% slot headroom against 62-81% for
  everything else, and it is a *shipped* board. It is the one to watch as `shared/core` lands.

### `s3-e1004` is blocked, not forgotten

`env:esp32-s3-E1004` pins `limengdu/bb_epaper` (PR bitbank2#32, T133A01); this repo vendors
`davelee98-creator/bb_epaper`, which defines neither `BBEP_T133A01` nor the
`EP133A_SPECTRA_1200x1600` enum that `display_service.cpp:700` maps onto. Writing the fragment
would produce a board that builds and cannot drive its own panel. It needs a re-vendor *and* a
fix to `e1004_write_stream_bytes()`, which calls `SPI.writeBytes()` with nothing calling
`SPI.begin()`. Full detail in [third_party/NOTICE.md](../../third_party/NOTICE.md).

## Partitions — ESP32-C6 moves to `min_spiffs.csv` (gate measured 2026-07-26)

**The gate below fired: the C6 image does NOT fit 1.25 MB.** Measured with ESP-IDF v5.5.4,
`idf.py -DOD_BOARD=c6-n4 build`:

| | bytes |
|---|---|
| C6 app image (`opendisplay.bin`) | **1 498 256** |
| `default_4MB.csv` slot (0x140000) | 1 310 720 → **overflows by 187 536** (14.3% over) |
| `min_spiffs_4MB.csv` slot (0x1E0000) | 1 966 080 → **fits, 467 824 spare** (24% free) |

So the C6 keeps dual-slot A/B on 4 MB, but pays for it with the filesystem: `partitions/
min_spiffs_4MB.csv`, 1.875 MB per slot and 128 KB of SPIFFS instead of 1.4 MB. Affordable
because config moved to NVS in phase B and nothing else on this target needs bulk storage.
`boards/c6-n4.cmake` points at it. The `default.csv` decision below is superseded.

The expectation recorded below — "the IDF image is expected to be *smaller* than today's
Arduino build" — was **not** borne out relative to the S3. The same tree builds to 1 241 056
bytes on `s3-n16r8` and 1 498 256 on `c6-n4`, i.e. the C6 is **257 KB larger while compiling
strictly less code** (no FastEPD, no PSRAM). Per-archive, the C6 pays:

- `libble_app.a` 235 592 vs the S3's `libbtdm_app.a` 89 867 — **+146 KB** for the C6's
  precompiled BLE controller. Single biggest item, and not something the app can shrink.
- WiFi/lwip stack +110 KB across `libpp` (+46 KB), `libnet80211` (+44 KB), `liblwip` (+19 KB).
- RISC-V code density: `libmain.a` is 129 756 on C6 vs 129 293 on S3 — *the same size despite
  the C6 not compiling FastEPD at all.*

(Ignore `libesp_app_format.a`'s ~105 KB in `esp-idf-size --archives`. It is the merged
`.rodata` string pool attributed to whichever object lands first in `.flash.rodata`; the map
shows `0xee (size before relaxing)` for it. Both chips show it. It is not that component.)

### Original decision (2026-07-25), superseded above

C6 units ship today on `huge_app.csv`: a single 3 MB app slot and **no OTA capability at all**.
Since the deployed-fleet migration is flash-and-reconfigure (MIGRATION.md § Deployed fleet
status), every C6 is on the bench once — and that is the only opportunity to give it a dual-slot
layout. **`default.csv`** is the decision:

```
# default.csv (4 MB)
app0,   app,  ota_0,  0x10000,  0x140000    # 1.25 MB
app1,   app,  ota_1,  0x150000, 0x140000    # 1.25 MB
spiffs, data, spiffs, 0x290000, 0x160000    # 1.4 MB
```

**Verify before rollout: the C6 IDF image must fit in 1.25 MB.** That is the tighter dual-slot
option — chosen for its 1.4 MB filesystem — and far less headroom than the 3 MB the app enjoys
today. Measuring it is a Phase B gate, not a rollout-time discovery.

- If it exceeds 1.25 MB → `min_spiffs.csv` gives 1.875 MB per slot, costing the filesystem
  (128 KB remains). Viable if nothing needs bulk storage, since config lives in NVS.
- If it exceeds 1.875 MB → C6 cannot do A/B on 4 MB at all, and is bench-only permanently.
  Record that as a finding rather than rediscovering it per unit.

The IDF image is expected to be *smaller* than today's Arduino build — dropping the Arduino core
and NimBLE-Arduino's wrapper typically shrinks it — so 1.25 MB is plausible. Plausible is not
measured.

The S3 boards need no change: `default_8/16/32MB.csv` already carry `app0` + `app1`.

## Layout (planned)

```
sdkconfig.defaults              common to all boards
sdkconfig.defaults.<idf_target> per-chip, auto-selected by IDF
boards/<board>.conf             per-board fragment
partitions/*.csv                per flash size
build.sh                        build.sh <board> — mirrors the Zephyr target's build.sh
compat/arduino_compat.h         TEMPORARY import shim; deleted during phase C
```

## Toolchain

ESP-IDF, pinned to one explicit release. Floors: **≥ 5.1** for ESP32-C6, **≥ 5.2** for
`driver/i2c_master.h` (do not port onto the deprecated `driver/i2c.h`).

**Installed on the primary dev box: ESP-IDF v5.5.4** at `~/esp/esp-idf`, activated with
`source ~/esp/esp-idf/export.sh` (it is not on `PATH`). That clears both floors and is the
obvious pin — adopt it explicitly in Phase B rather than depending on whatever a given machine
has exported.
