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
                           ${CMAKE_CURRENT_LIST_DIR}/../compat/wifi_compat.cpp
                           ${CMAKE_CURRENT_LIST_DIR}/../compat/app_main.cpp)

# Target HAL implementations. od_hal_nvs is the first: config storage moved off LittleFS onto
# NVS (2026-07-25), written to the shared/hal signature so promotion is a repoint.
# od_hal_log is the second (2026-08-04, phase C): it owns the log PORT so od_log.h could stop
# including <Arduino.h>. Its backend is a compile-time choice on OPENDISPLAY_LOG_UART.
# od_hal_gpio and od_hal_time are the third (2026-08-04, phase C step 3): the pin/clock seam
# the power-latch, buzzer and button files share. Signatures are SHARED_API_DESIGN.md's.
list(APPEND OD_APP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_nvs.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_log.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_gpio.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_time.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_i2c.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_adc.c
                           ${CMAKE_CURRENT_LIST_DIR}/../ble/od_ble_nimble.cpp)

# ble/od_ble_rx.cpp is GONE, not forgotten. It held od_ble_on_write(): the bounds check,
# the RX hex log and the enqueue. All three now live in src/command_queue.cpp's
# bleRxQueuePush(), which both targets share -- keeping a second copy here is what let the
# two transports report the same malformed frame differently.

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

# bb_epaper.cpp is EXCLUDED and replaced by ../panel/od_bbep.cpp.
#
# That file is 827 lines: 52 of glue (includes, the *_io.inl selection #ifdef chain, then
# bb_ep.inl + bb_ep_gfx.inl) and 775 of the BBEPAPER C++ class, which this project never
# references -- display_service.cpp drives a raw BBEPDISP through the bare C functions.
#
# Replacing the glue rather than patching its #ifdef chain (how Firmware_NRF54 selects its
# backend) means ZERO edits to vendored files to use our own IO backend, so nothing new joins
# third_party/NOTICE.md's re-verify-on-every-bump list. Dropping the class also removes the only
# consumer of pinMode() and millis(), which is why panel/od_bbep_idf_io.inl defines neither.
#
# Group5.cpp stays: it is a separate TU providing the G5 codec, and the FastEPD collision
# handling below still applies to it.
#
# See docs/BBEPAPER_IO_BACKENDS.md and panel/od_bbep.cpp.
list(FILTER OD_BBEP_SOURCES EXCLUDE REGEX "/bb_epaper\\.cpp$")
list(APPEND OD_APP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/../panel/od_bbep.cpp)

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
# Boards whose source env sets -DOPENDISPLAY_FASTEPD *and* lists the FastEPD lib_dep.
# Read from platformio.ini, not guessed -- the two are not the same set:
#
#   s3-e1004 was WRONG in this list. env:esp32-s3-E1004 extends env:esp32-s3-N32R8-extuart,
#   which sets neither the define nor the lib_dep; that board's panel goes through bb_epaper.
#   Listing it here would have given it FastEPD's Group5 and excluded bb_epaper's, changing
#   which copy wins the collision resolved below.
#
#   s3-n8r8 and the two s3-n16r8-extuart boards were MISSING. All three set the define.
set(OD_FASTEPD_BOARDS
    s3-n16r8
    s3-n8r8
    s3-n32r8
    s3-n16r8-extuart
    s3-n16r8-extuart-debug)

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
        ${CMAKE_CURRENT_LIST_DIR}/../panel
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

# FastEPD selects its IO backend with `#ifdef ARDUINO / #elif defined(__LINUX__)`. Under IDF
# NEITHER was defined, so every it8951 transport function in FastEPD.inl compiled to an EMPTY
# BODY between two gpio_set_level() calls: commands sent nothing, pixel writes sent nothing,
# and it8951ReadData() returned 0 -- while the GPIO reset/busy handshake still looked healthy.
# It linked and it booted; the panel simply never received a byte.
#
# OD_FASTEPD_IDF_SPI selects a THIRD branch, added as an OD-PATCH in FastEPD.inl, that drives
# the transport through compat/SPI.h. Defining ARDUINO instead was tried and rejected: it also
# selects FastEPD's Arduino font/Print/PROGMEM surface (pgm_read_*, memcpy_P, ledcAttach,
# TwoWire::setTimeout), which is a whole Arduino core rather than an IO backend, and bb_epaper
# reads the same macro and must keep its esp_idf backend.
#
# Both this define and the patch disappear when FastEPD gains a real esp_idf IO backend
# upstream. See third_party/NOTICE.md § "FastEPD has no ESP-IDF IO backend".
if(OD_BOARD IN_LIST OD_FASTEPD_BOARDS)
    set_source_files_properties(${OD_TP_DIR}/FastEPD/src/FastEPD.cpp
        TARGET_DIRECTORY ${COMPONENT_LIB}
        PROPERTIES COMPILE_DEFINITIONS "OD_FASTEPD_IDF_SPI=1")
endif()

# The imported sources are Arduino C++ and will not be warning-clean under IDF's defaults for
# some time. Phase B's goal is to LINK AND BOOT, so warnings stay warnings here -- unlike
# shared/, which is -Werror from its first line (tests/host/CMakeLists.txt). Tightening this
# is phase C work, and it should be tightened: leaving it permanently is how the imported half
# of the tree stays permanently unowned.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function)
