# Target: ESP32-S3 / C3 / C6 / classic (ESP-IDF)

Source repo: `Firmware` (https://github.com/davelee98/Firmware)

Not yet imported — build from the original repo for now. First target in the migration order;
see ../../docs/MIGRATION.md.

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
`driver/i2c_master.h` (do not port onto the deprecated `driver/i2c.h`). Not currently installed
on the primary dev box — `idf.py` must be installed before any work here.
