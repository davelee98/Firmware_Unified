# ESP32-C6, 4 MB flash, no PSRAM. Translated from platformio.ini env:esp32-c6-N4.
# Source of the mapping: ../../docs/TOOLCHAINS.md § "PlatformIO knob -> sdkconfig".
set(OD_IDF_TARGET esp32c6)
set(OD_PARTITION_CSV partitions/min_spiffs_4MB.csv)
set(OD_SDKCONFIG_FRAGMENTS sdkconfig.defaults.esp32c6 boards/c6-n4.sdkconfig)

# Differences from the S3 fragment, all of them hardware facts rather than preferences:
#
#   * no BOARD_HAS_PSRAM  -- the C6 has no PSRAM controller at all.
#   * no OPENDISPLAY_FASTEPD -- FastEPD drives the S3's parallel LCD peripheral, which the C6
#     does not have, and it is PSRAM-mandatory besides (MEMORY_CONSTRAINTS.md § "FastEPD is
#     ESP32-only and PSRAM-mandatory"). The source env:esp32-c6-N4 does not set it either.
#
# The partition table is NOT the source env's huge_app.csv: shipped C6 units get a dual-slot
# layout while they are on the bench (README.md § Partitions). It is min_spiffs_4MB.csv rather
# than default_4MB.csv because the gate MEASURED 1 498 256 bytes against default_4MB.csv's
# 1 310 720 slot -- a 187 536-byte overflow. See partitions/min_spiffs_4MB.csv for the numbers.
# ARDUINO_USB_* become a console choice in sdkconfig.defaults.esp32c6, not a define.
set(OD_BOARD_DEFINES
    TARGET_ESP32
    OPENDISPLAY_ENABLE_WIFI
    OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1
)
