# Classic ESP32 (esp32dev), 4 MB flash, no PSRAM. From platformio.ini env:esp32-N4.
#
# The odd one out in two ways, both from the source env:
#
#   * NO OPENDISPLAY_ENABLE_WIFI. Every other ESP target carries the WiFi/LAN transport;
#     this one is explicitly excluded ("S3 + C6 + C3", classic ESP32 excluded).
#   * PIPE_SMALL_DRAM_WINDOW. 320 KB RAM leaves far less static DRAM than the S3/C3/C6
#     parts, so the full 33-slot PIPE_WRITE reorder queue overflows dram0_0_seg at link.
#     structs.h shrinks the window and queue on this define alone. This is the board
#     NEXT_STEPS.md item 3 names as the one to expect trouble from.
set(OD_IDF_TARGET esp32)
set(OD_PARTITION_CSV partitions/min_spiffs_4MB.csv)
set(OD_SDKCONFIG_FRAGMENTS sdkconfig.defaults.esp32 boards/esp32-n4.sdkconfig)

set(OD_BOARD_DEFINES
    TARGET_ESP32
    PIPE_SMALL_DRAM_WINDOW
    # Egress depth follows the window it has to cover: PIPE_MAX_W + 2, so usable capacity
    # (SLOTS - 1) holds a full window plus its END. Sized here rather than left at the shared
    # default because that default covers W=32 and would keep ~4 KB of TX storage this board
    # can never fill. structs.h asserts the relationship.
    OD_TXQ_SLOTS=18u
    OD_RXQ_SLOTS=18u
    OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1
)
