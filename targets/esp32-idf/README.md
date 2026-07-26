# Target: ESP32-S3 / C3 / C6 / classic (ESP-IDF)

Source repo: `Firmware` (https://github.com/OpenDisplay/Firmware.git)

## Import status: **Phase A complete — does not build**

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

Ten variants from the source repo's `platformio.ini`:

| Board | Chip | Flash / PSRAM | Notes |
|---|---|---|---|
| `s3-n8r8` / `s3-n16r8` / `s3-n32r8` | ESP32-S3 | 8/16/32 MB + 8 MB OPI PSRAM | `Seeed_GFX` path on N16R8/N32R8 |
| `s3-n16r8-extuart` / `s3-n32r8-extuart` | ESP32-S3 | as above | console on CH343P UART (GPIO43/44), not USB-CDC |
| `s3-e1004` | ESP32-S3 | 32 MB + 8 MB | Seeed reTerminal E1004; pinned bb_epaper for T133A01 |
| `c3-n4` / `c3-n16` | ESP32-C3 | 4/16 MB, no PSRAM | `c3-n16` uses DIO flash to free GPIO12/13 |
| `c6-n4` | ESP32-C6 | 4 MB, no PSRAM | requires IDF ≥ 5.1. **Shipped.** Partition: `default.csv` — see below |
| `esp32-n4` | classic ESP32 | 4 MB, no PSRAM | 320 KB RAM: needs the reduced PIPE reorder window |

## Partitions — ESP32-C6 moves to `default.csv` (decided 2026-07-25)

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
