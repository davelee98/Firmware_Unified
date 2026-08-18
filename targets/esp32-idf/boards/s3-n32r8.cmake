# ESP32-S3, 32 MB flash (W25Q256), 8 MB OPI PSRAM. From platformio.ini env:esp32-s3-N32R8.
# Same software configuration as s3-n16r8; only the flash size and partition table differ.
set(OD_IDF_TARGET esp32s3)
set(OD_PARTITION_CSV partitions/default_32MB.csv)
set(OD_SDKCONFIG_FRAGMENTS sdkconfig.defaults.esp32s3 boards/s3-n32r8.sdkconfig)

set(OD_BOARD_DEFINES
    OPENDISPLAY_ENABLE_WIFI
    BOARD_HAS_PSRAM
    OPENDISPLAY_FASTEPD
)
