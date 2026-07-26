# ESP32-S3, 16 MB flash, 8 MB OPI PSRAM. Translated from platformio.ini env:esp32-s3-N16R8.
# Source of the mapping: ../../docs/TOOLCHAINS.md § "PlatformIO knob -> sdkconfig".
set(OD_IDF_TARGET esp32s3)
set(OD_PARTITION_CSV partitions/default_16MB.csv)
set(OD_SDKCONFIG_FRAGMENTS sdkconfig.defaults.esp32s3 boards/s3-n16r8.sdkconfig)

# ARDUINO_USB_* have no IDF equivalent as defines -- USB-CDC-on-boot is a sdkconfig choice
# (CONFIG_ESP_CONSOLE_USB_CDC), so it lives in the fragment, not here. Carrying the Arduino
# spelling across would be exactly the "build_flags idiom" CLAUDE.md forbids.
set(OD_BOARD_DEFINES
    TARGET_ESP32
    OPENDISPLAY_ENABLE_WIFI
    OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1
    BOARD_HAS_PSRAM
    OPENDISPLAY_FASTEPD
)
