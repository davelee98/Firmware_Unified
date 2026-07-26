# The application component.
#
# The imported sources live in ../src rather than in this directory, deliberately: phase A put
# them where the source repo had them so the import diff is a pure copy and blame stays
# readable. Registering them from here costs one relative path and keeps that property.

set(OD_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../src)
set(OD_LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/../lib)

file(GLOB OD_APP_SOURCES
    ${OD_SRC_DIR}/*.cpp
    ${OD_SRC_DIR}/*.c
    ${OD_SRC_DIR}/qr/*.c
)

# uzlib + the resumable inflate engine. od_zlib_stream.c is destined for shared/compress/ at
# migration step 3-4 (SHARED_API_DESIGN.md); it is compiled from lib/ for now so that the
# promotion is a separate, revertable commit rather than something smuggled into phase B.
list(APPEND OD_APP_SOURCES
    ${OD_LIB_DIR}/uzlib/src/od_zlib_stream.c
    ${OD_LIB_DIR}/uzlib/src/adler32.c
    ${OD_LIB_DIR}/uzlib/src/crc32.c
)

# The Arduino shim. TEMPORARY -- removed in phase C together with compat/ and its ratchet.
list(APPEND OD_APP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/../compat/arduino_compat.cpp
                           ${CMAKE_CURRENT_LIST_DIR}/../compat/wifi_compat.cpp)

# Target HAL implementations. od_hal_nvs is the first: config storage moved off LittleFS onto
# NVS (2026-07-25), written to the shared/hal signature so promotion is a repoint.
list(APPEND OD_APP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_nvs.c
                           ${CMAKE_CURRENT_LIST_DIR}/../ble/od_ble_nimble.cpp)

# ---------------------------------------------------------------------------------------
# Panel libraries, vendored -- see ../../../third_party/NOTICE.md for origin and licence.
# ---------------------------------------------------------------------------------------
set(OD_TP_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../third_party)

# The Group5 collision, resolved by source selection rather than by a linker flag.
#
# bb_epaper and FastEPD both ship Group5.cpp -- same author, diverged copies, and NOT the same
# licence header (bb_epaper's carries SPDX GPL-3.0-or-later; FastEPD's does not). The source
# repo linked both and passed -Wl,--allow-multiple-definition, which makes the effective G5
# codec depend on link order and hides any divergence between the copies.
#
# MEMORY_CONSTRAINTS.md § "Open questions" recommends option 1 -- compile the duplicated file
# from ONE library -- and DESIGN_REVIEW § "Likely pitfalls" #4 warns the flag must not ship
# "temporarily", because temporary linker workarounds become permanent ones. So: on a FastEPD
# board, Group5 comes from FastEPD and bb_epaper's copy is excluded; otherwise only bb_epaper
# is compiled and the question does not arise. No --allow-multiple-definition anywhere.
file(GLOB OD_BBEP_SOURCES ${OD_TP_DIR}/bb_epaper/src/*.cpp)

# Which boards use FastEPD, keyed on OD_BOARD alone.
#
# This mapping would sit more naturally in boards/<board>.cmake, and it is here for a reason
# worth recording: IDF expands component CMakeLists in a SEPARATE early cmake invocation whose
# cache carries none of the parent project's variables -- not OD_BOARD_DEFINES, not even a
# CACHE INTERNAL set by the parent. A conditional source list keyed on those silently takes
# the wrong branch in that pass. Including the board fragment here instead makes IDF's own
# property serialisation emit a dev warning, which IDF then treats as a fatal error.
#
# OD_BOARD is a -D on the command line, so it is in the cache from the first pass onward and
# both passes agree. FastEPD is ESP32-S3-only and PSRAM-mandatory (MEMORY_CONSTRAINTS.md
# § "FastEPD is ESP32-only and PSRAM-mandatory"), so this list can only ever hold S3 boards.
set(OD_FASTEPD_BOARDS s3-n16r8 s3-n32r8 s3-e1004)

if(OD_BOARD IN_LIST OD_FASTEPD_BOARDS)
    list(APPEND OD_APP_SOURCES ${OD_TP_DIR}/FastEPD/src/FastEPD.cpp
                               ${OD_TP_DIR}/FastEPD/src/Group5.cpp)
    list(FILTER OD_BBEP_SOURCES EXCLUDE REGEX "/Group5\\.cpp$")
    list(APPEND OD_APP_SOURCES ${OD_BBEP_SOURCES})
    message(STATUS "od: FastEPD board -- Group5 from FastEPD, bb_epaper's copy excluded")
else()
    list(APPEND OD_APP_SOURCES ${OD_BBEP_SOURCES})
    message(STATUS "od: non-FastEPD board -- Group5 from bb_epaper")
endif()

idf_component_register(
    SRCS ${OD_APP_SOURCES}
    INCLUDE_DIRS
        ${OD_SRC_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../compat
        ${CMAKE_CURRENT_LIST_DIR}/../hal
        ${CMAKE_CURRENT_LIST_DIR}/../ble
        ${OD_LIB_DIR}/uzlib/src
        ${CMAKE_CURRENT_LIST_DIR}/../../../third_party/bb_epaper/src
        ${CMAKE_CURRENT_LIST_DIR}/../../../third_party/FastEPD/src
        # The wire contract, consumed directly from shared/ -- NOT copied into this target.
        # This is the whole reason phase A left include/opendisplay_protocol.h behind.
        ${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol
    REQUIRES
        bt nvs_flash esp_timer driver esp_wifi esp_event mbedtls spiffs vfs esp_adc
        # esp_lcd: FastEPD drives the S3 LCD peripheral for parallel e-paper
        esp_lcd esp_psram
)

# Board defines from boards/<board>.cmake. These are the surviving half of the source repo's
# build_flags: the -D flags that are genuinely application configuration, as opposed to the
# Arduino/PlatformIO plumbing (ARDUINO_USB_*, board_build.*) which became sdkconfig instead.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${OD_BOARD_DEFINES})

# The imported sources are Arduino C++ and will not be warning-clean under IDF's defaults for
# some time. Phase B's goal is to LINK AND BOOT, so warnings stay warnings here -- unlike
# shared/, which is -Werror from its first line (tests/host/CMakeLists.txt). Tightening this
# is phase C work, and it should be tightened: leaving it permanently is how the imported half
# of the tree stays permanently unowned.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function)
