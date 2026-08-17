# s3-n32r8-extuart with DEBUG-level logging compiled in. From
# platformio.ini env:esp32-s3-N32R8-extuart-debug.
#
# -extuart builds print no panic backtrace over USB (logging goes out GPIO43/44), so
# debug-level logs are the only running commentary during panel bring-up -- same rationale
# as s3-n16r8-extuart-debug, upstream's other bench-only debug env.
set(OD_IDF_TARGET esp32s3)
set(OD_PARTITION_CSV partitions/default_32MB.csv)
set(OD_SDKCONFIG_FRAGMENTS
    sdkconfig.defaults.esp32s3
    boards/s3-n32r8.sdkconfig
    boards/_extuart.sdkconfig)

set(OD_BOARD_DEFINES
    OPENDISPLAY_ENABLE_WIFI
    OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1
    BOARD_HAS_PSRAM
    OPENDISPLAY_LOG_UART
    OPENDISPLAY_LOG_UART_RX=44
    OPENDISPLAY_LOG_UART_TX=43
    OD_LOG_LEVEL=OD_LOG_DEBUG
)
