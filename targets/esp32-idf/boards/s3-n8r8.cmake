# ESP32-S3, 8 MB flash, 8 MB OPI PSRAM. Translated from platformio.ini env:esp32-s3-N8R8.
# Identical to s3-n16r8 except for flash size and therefore the partition table.
set(OD_IDF_TARGET esp32s3)
set(OD_PARTITION_CSV partitions/default_8MB.csv)
set(OD_SDKCONFIG_FRAGMENTS sdkconfig.defaults.esp32s3 boards/s3-n8r8.sdkconfig)

set(OD_BOARD_DEFINES
    TARGET_ESP32
    OPENDISPLAY_ENABLE_WIFI
    OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1
    BOARD_HAS_PSRAM
    OPENDISPLAY_FASTEPD
)
