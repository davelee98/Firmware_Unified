# s3-n16r8-extuart with DEBUG-level logging compiled in. From
# platformio.ini env:esp32-s3-N16R8-extuart-debug.
#
# OD_LOG_LEVEL defaults to INFO, which compiles every od_log_debug() away -- so the config
# parse dump and summary, the BLE RX/TX hex lines, and the DIRECT_WRITE throughput lines are
# ABSENT from a normal build. This turns them on. It costs flash and real serial time, so it
# is a debug build, not a shipping one.
set(OD_IDF_TARGET esp32s3)
set(OD_PARTITION_CSV partitions/default_16MB.csv)
set(OD_SDKCONFIG_FRAGMENTS
    sdkconfig.defaults.esp32s3
    boards/s3-n16r8.sdkconfig
    boards/_extuart.sdkconfig)

set(OD_BOARD_DEFINES
    OPENDISPLAY_ENABLE_WIFI
    BOARD_HAS_PSRAM
    OPENDISPLAY_FASTEPD
    OPENDISPLAY_LOG_UART
    OPENDISPLAY_LOG_UART_RX=44
    OPENDISPLAY_LOG_UART_TX=43
    OD_LOG_LEVEL=OD_LOG_DEBUG
)
