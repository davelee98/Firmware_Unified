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
