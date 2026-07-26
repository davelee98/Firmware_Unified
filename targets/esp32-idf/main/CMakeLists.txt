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
list(APPEND OD_APP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/../compat/arduino_compat.cpp)

idf_component_register(
    SRCS ${OD_APP_SOURCES}
    INCLUDE_DIRS
        ${OD_SRC_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../compat
        ${OD_LIB_DIR}/uzlib/src
        # The wire contract, consumed directly from shared/ -- NOT copied into this target.
        # This is the whole reason phase A left include/opendisplay_protocol.h behind.
        ${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol
    REQUIRES
        bt nvs_flash esp_timer driver esp_wifi esp_event mbedtls spiffs vfs esp_adc
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
