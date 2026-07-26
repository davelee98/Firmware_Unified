# ESP32-C3, 4 MB flash, no PSRAM. From platformio.ini env:esp32-c3-N4.
#
# No BOARD_HAS_PSRAM (the C3 has no PSRAM controller) and no OPENDISPLAY_FASTEPD (FastEPD
# drives the S3's parallel LCD peripheral and is PSRAM-mandatory besides). The source env
# sets neither.
#
# Partition table is min_spiffs_4MB.csv rather than the source env's huge_app.csv, for the
# same reason c6-n4 uses it: shipped 4 MB units get a dual A/B layout, and the C6 gate
# already proved a 1.25 MB slot too small for an IDF image of this shape.
set(OD_IDF_TARGET esp32c3)
set(OD_PARTITION_CSV partitions/min_spiffs_4MB.csv)
set(OD_SDKCONFIG_FRAGMENTS sdkconfig.defaults.esp32c3 boards/c3-n4.sdkconfig)

set(OD_BOARD_DEFINES
    TARGET_ESP32
    OPENDISPLAY_ENABLE_WIFI
    OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1
)
