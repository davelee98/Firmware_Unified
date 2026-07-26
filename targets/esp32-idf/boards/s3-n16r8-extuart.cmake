# s3-n16r8 with the log on the onboard CH343P USB-UART (GPIO43/44) instead of USB-CDC.
# From platformio.ini env:esp32-s3-N16R8-extuart. Keeps FastEPD -- unlike the N32R8 extuart
# board, this env DOES list the FastEPD lib_dep and set OPENDISPLAY_FASTEPD.
set(OD_IDF_TARGET esp32s3)
set(OD_PARTITION_CSV partitions/default_16MB.csv)
set(OD_SDKCONFIG_FRAGMENTS
    sdkconfig.defaults.esp32s3
    boards/s3-n16r8.sdkconfig
    boards/_extuart.sdkconfig)

set(OD_BOARD_DEFINES
    TARGET_ESP32
    OPENDISPLAY_ENABLE_WIFI
    OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1
    BOARD_HAS_PSRAM
    OPENDISPLAY_FASTEPD
    OPENDISPLAY_LOG_UART
    OPENDISPLAY_LOG_UART_RX=44
    OPENDISPLAY_LOG_UART_TX=43
)
