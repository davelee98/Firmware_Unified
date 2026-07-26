# Generic 16 MB ESP32-C3, no PSRAM. From platformio.ini env:esp32-c3-N16.
#
# All board specifics -- panel, pins, and the optional battery latch via
# DEVICE_FLAG_BATTERY_LATCH -- come from the runtime device config, so there is nothing
# board-specific to express here beyond the flash geometry.
#
# The source env sets board_build.flash_mode = dio deliberately: DIO leaves GPIO12/13 free
# for general GPIO use (e.g. a battery-latch MOSFET). That is carried into the sdkconfig
# fragment, where flash mode lives under IDF.
set(OD_IDF_TARGET esp32c3)
set(OD_PARTITION_CSV partitions/default_16MB.csv)
set(OD_SDKCONFIG_FRAGMENTS sdkconfig.defaults.esp32c3 boards/c3-n16.sdkconfig)

set(OD_BOARD_DEFINES
    TARGET_ESP32
    OPENDISPLAY_ENABLE_WIFI
    OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1
)
