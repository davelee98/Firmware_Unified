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
| `c6-n4` | ESP32-C6 | 4 MB, no PSRAM | requires IDF ≥ 5.1 |
| `esp32-n4` | classic ESP32 | 4 MB, no PSRAM | 320 KB RAM: needs the reduced PIPE reorder window |

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
