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
    # -DOPENDISPLAY_ENABLE_WIFI deliberately absent: no PSRAM here. BLE-only (no LAN push,
    # mDNS or TLS-PSK), and the inflate engine falls back to uzlib.
    #
    # The flag has exactly two consumers -- OPENDISPLAY_HAS_WIFI (src/wifi_service.h) and
    # OPENDISPLAY_USE_TINFL (src/od_inflate_tinfl.h) -- so dropping it here drops both the LAN
    # transport and tinfl's ~11 KB of .data tables from a part with no PSRAM to relocate them
    # into. Set it ONLY on boards that also set BOARD_HAS_PSRAM; nothing in code enforces that.
    #
    # This is the boards/*.cmake half of Firmware's dc60c8a, whose own half was three
    # per-env edits to platformio.ini -- a file this repo deliberately does not carry, so the
    # change could not arrive with the source sync and had to be re-expressed here.
)
