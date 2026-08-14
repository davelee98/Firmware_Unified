cmake_minimum_required(VERSION 3.13.1)

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
# shared/ -- ONE source list, four consumers (shared/sources.cmake). Sets OD_SHARED_SOURCES,
# the per-tier lists, and OD_SHARED_INCLUDE_DIRS. Do not glob shared/ from this file.
include(${PROJECT_ROOT}/../../shared/sources.cmake)
# third_party is the REPO-LEVEL vendored tree, shared by every target -- not a per-target copy.
# The source repo had its own third_party/ beside src/; importing that would have made this
# repo's "one vendored copy for all targets" rule a fiction. ../../.. is the repo root.
set(THIRD_PARTY_DIR ${PROJECT_ROOT}/../../third_party)
set(PANEL_DIR ${PROJECT_ROOT}/panel)
set(SRC_DIR ${PROJECT_ROOT}/src)
set(GEN_DIR ${SRC_DIR}/generated)

list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_ncs.conf)

# NO prj_cs.conf HERE. Channel Sounding is an nRF54L radio feature and the nRF52840 has none, so
# appending it unconditionally made every CS symbol report "assigned y but got n" -- fatal in a
# build that promotes Kconfig warnings to errors. It now lives in boards/<board>.conf for the two
# nRF54 boards.
#
# GUARDING THE APPEND ON ${BOARD} DOES NOT WORK, and the attempt is worth recording because the
# failure is silent: BOARD is EMPTY here. find_package(Zephyr) sets it, and that runs on line 41,
# after EXTRA_CONF_FILE has been assembled. `if(NOT BOARD MATCHES ...)` therefore always takes
# the true branch. The pre-existing lm20 guard below has exactly the same defect -- it has never
# fired -- which is why prj_lm20_extra.conf's content is ALSO mirrored into
# boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.conf. That mirroring is what has been carrying it.

# DEAD BRANCH, left only as a marker: BOARD is empty here (see above), so this has never fired.
# Its content is applied via boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.conf, whose own header
# already said "also mirrored ... for sysbuild". Removing the file it references would be the
# tidier fix; it is left in place so this change alters no nRF54 behaviour.
if(BOARD MATCHES ".*lm20.*")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_lm20_extra.conf)
endif()

if(DEFINED ENV{PROFILE} AND "$ENV{PROFILE}" STREQUAL "uart")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_uart.conf)
elseif(OD_UART_DEBUG OR (DEFINED PROFILE AND PROFILE STREQUAL "uart"))
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_uart.conf)
endif()

if(DEFINED ENV{PROFILE} AND "$ENV{PROFILE}" STREQUAL "quiet")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_quiet.conf)
  set(OD_LOW_POWER_QUIET ON)
elseif(OD_LOW_POWER_QUIET OR (DEFINED PROFILE AND PROFILE STREQUAL "quiet"))
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_quiet.conf)
  set(OD_LOW_POWER_QUIET ON)
endif()

if(DEFINED ENV{PROFILE} AND "$ENV{PROFILE}" STREQUAL "debug")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_debug.conf)
  set(OD_DEBUG_BUILD ON)
elseif(OD_DEBUG_BUILD OR (DEFINED PROFILE AND PROFILE STREQUAL "debug"))
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_debug.conf)
  set(OD_DEBUG_BUILD ON)
endif()

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opendisplay_nordic)

set(FACTORY_GEN_SCRIPT ${PROJECT_ROOT}/scripts/factory_config_gen.py)
set(FACTORY_GEN_STAMP ${GEN_DIR}/.factory_gen_stamp)

if(NOT DEFINED FACTORY_CONFIG_HEX)
  set(FACTORY_CONFIG_HEX "")
endif()
if(NOT DEFINED FACTORY_CLEAR_CONFIG_ON_BOOT)
  set(FACTORY_CLEAR_CONFIG_ON_BOOT OFF)
endif()

set(FACTORY_GEN_ARGS "")
if(FACTORY_CONFIG_HEX)
  list(APPEND FACTORY_GEN_ARGS --hex=${FACTORY_CONFIG_HEX})
endif()
if(FACTORY_CLEAR_CONFIG_ON_BOOT)
  list(APPEND FACTORY_GEN_ARGS --clear)
endif()

add_custom_command(
  OUTPUT ${FACTORY_GEN_STAMP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN_DIR}
  COMMAND ${Python3_EXECUTABLE} ${FACTORY_GEN_SCRIPT} ${FACTORY_GEN_ARGS}
  COMMAND ${CMAKE_COMMAND} -E touch ${FACTORY_GEN_STAMP}
  DEPENDS ${FACTORY_GEN_SCRIPT} ${PROJECT_ROOT}/tools/config_packet.py
  WORKING_DIRECTORY ${PROJECT_ROOT}
  COMMENT "Generating factory_config_data.c"
  VERBATIM
)

add_custom_target(factory_config_gen DEPENDS ${FACTORY_GEN_STAMP})

# OD_SHARED_INCLUDE_DIRS FIRST, and that is a rule rather than a formatting choice: it carries
# the canonical opendisplay_protocol.h / opendisplay_structs.h. ${SRC_DIR} used to carry
# hand-written subsets of both, which shadowed them; those are gone (src/od_runtime_types.h now
# holds the RAM-only types they also carried), and keeping shared/ ahead of src/ is what stops
# another copy from quietly taking over if one reappears.
target_include_directories(app PRIVATE
  ${OD_SHARED_INCLUDE_DIRS}
  ${SRC_DIR}
  ${THIRD_PARTY_DIR}/bb_epaper/src
  ${PANEL_DIR}
  ${THIRD_PARTY_DIR}/uzlib/src
  ${GEN_DIR}
)

target_compile_definitions(app PRIVATE
  OD_APP_VERSION=0x0100
  OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=0
)

# BOARD *is* set here -- find_package(Zephyr) has run. Only the EXTRA_CONF_FILE block above it
# sees an empty BOARD; do not move these down-stream checks up there.
#
# The nRF52840 needed its own arm. This was a two-way if, so xiao_ble fell into the else and
# compiled as the L15 board -- meaning early init drove P2.3/P2.5/P2.10, which
# are nRF54L15 RF-switch and boost-select pins that DO NOT EXIST on the nRF52840 (P0/P1 only).
# It was silent rather than fatal because the pin decoder rejects a port whose gpio node is
# not okay and returns false, so every one of those writes was a no-op. Silent-and-wrong is
# still wrong: the board got no init of its own.
if(CONFIG_OD_PLATFORM_NRF52840)
  target_compile_definitions(app PRIVATE
    OD_PLATFORM_NRF52840
    OD_BOARD_XIAO_NRF52840
    OD_PLATFORM_NAME=\"nRF52840\"
    OPENDISPLAY_BUILD_ID=\"nrf52840\")
elseif(CONFIG_OD_PLATFORM_NRF54)
  target_compile_definitions(app PRIVATE
    OD_PLATFORM_NRF54
    OD_PLATFORM_NAME=\"nRF54\"
    OPENDISPLAY_BUILD_ID=\"nrf54\")
  if(CONFIG_SOC_NRF54LM20A)
    target_compile_definitions(app PRIVATE OD_BOARD_NRF54LM20A)
  elseif(CONFIG_SOC_NRF54L15)
    target_compile_definitions(app PRIVATE OD_BOARD_NRF54L15)
  else()
    message(FATAL_ERROR "Unsupported nRF54 SoC")
  endif()
else()
  message(FATAL_ERROR "Unsupported Nordic platform: expected nRF52840 or nRF54L")
endif()

# Application verbosity is a build-profile property, independent of Zephyr subsystem levels.
# Both profiles use the same OpenDisplay record format and native raw-log transport.
if(OD_DEBUG_BUILD)
  target_compile_definitions(app PRIVATE OD_DEBUG_BUILD=1 OD_LOG_LEVEL=OD_LOG_DEBUG)
else()
  target_compile_definitions(app PRIVATE OD_LOG_LEVEL=OD_LOG_INFO)
endif()

# Default BLE TX power in dBm, used when the stored config leaves PowerOption.tx_power at 0.
#
# THIS IS THE ONLY BUILD-TIME KNOB THAT REACHES THE RADIO ON THIS TARGET. Zephyr's
# BT_CTLR_TX_PWR_* choice looks like the obvious one and is inert here: CONFIG_BT_LL_SOFTDEVICE
# selects the SoftDevice Controller, and BT_CTLR_TX_PWR_DBM has no consumer outside Zephyr's own
# software link layer (BT_LL_SW_SPLIT). Power is set at runtime instead, by the HCI VS
# Write_Tx_Power_Level that opendisplay_ble.c's apply_tx_power() issues -- so the default has to
# reach that call, not Kconfig.
#
# +8 dBm is the maximum on every board this target builds: nRF52840 sets
# HAS_HW_NRF_RADIO_TX_PWR_HIGH (radio-tx-high-power-supported in its DTS) and nRF54L15/LM20A are
# SOC_COMPATIBLE_NRF54LX. The controller clamps to the nearest supported step and reports what it
# took; apply_tx_power() logs requested and selected so the two are never assumed equal.
if(NOT DEFINED OD_TX_POWER_DBM)
  set(OD_TX_POWER_DBM 8)
endif()
target_compile_definitions(app PRIVATE OD_TX_POWER_DBM=${OD_TX_POWER_DBM})

# OpenDisplay app version string (boot screen + CMD_FIRMWARE_VERSION major.minor.patch).
# Prefer -DOD_FW_VERSION / -Dzephyr_OD_FW_VERSION; also accept ENV from build.sh
# because sysbuild does not always forward unprefixed cache vars into the app image.
set(_od_fw_ver "")
if(DEFINED OD_FW_VERSION AND NOT "${OD_FW_VERSION}" STREQUAL "")
  set(_od_fw_ver "${OD_FW_VERSION}")
elseif(DEFINED ENV{OD_FW_VERSION} AND NOT "$ENV{OD_FW_VERSION}" STREQUAL "")
  set(_od_fw_ver "$ENV{OD_FW_VERSION}")
elseif(DEFINED ENV{BUILD_VERSION} AND NOT "$ENV{BUILD_VERSION}" STREQUAL "")
  set(_od_fw_ver "$ENV{BUILD_VERSION}")
endif()
if(NOT "${_od_fw_ver}" STREQUAL "")
  target_compile_definitions(app PRIVATE OD_FW_VERSION=\"${_od_fw_ver}\")
  message(STATUS "OpenDisplay FW version: ${_od_fw_ver}")
endif()
# Prefer -DGIT_SHA / -Dzephyr_GIT_SHA (sysbuild strips zephyr_); also ENV{SHA}.
set(_od_git_sha "")
if(DEFINED GIT_SHA AND NOT "${GIT_SHA}" STREQUAL "")
  set(_od_git_sha "${GIT_SHA}")
elseif(DEFINED ENV{SHA} AND NOT "$ENV{SHA}" STREQUAL "")
  set(_od_git_sha "$ENV{SHA}")
endif()
if(NOT "${_od_git_sha}" STREQUAL "")
  target_compile_definitions(app PRIVATE SHA=\"${_od_git_sha}\")
endif()
if(FACTORY_CONFIG_HEX)
  target_compile_definitions(app PRIVATE FACTORY_HAS_EMBED=1)
endif()
if(FACTORY_CLEAR_CONFIG_ON_BOOT)
  target_compile_definitions(app PRIVATE FACTORY_CLEAR_CONFIG_ON_BOOT=1)
endif()
if(OD_LOW_POWER_QUIET)
  target_compile_definitions(app PRIVATE OD_LOW_POWER_QUIET=1)
endif()

target_sources(app PRIVATE
  ${SRC_DIR}/main.c
  ${SRC_DIR}/od_board_common.c
  ${SRC_DIR}/od_gpio.c
  ${SRC_DIR}/od_pin_codec.c
  ${SRC_DIR}/od_zephyr_time.c
  ${SRC_DIR}/newlib_stubs.c
  ${SRC_DIR}/od_log.c
  ${SRC_DIR}/opendisplay_ble.c
  ${SRC_DIR}/opendisplay_battery.c
  ${SRC_DIR}/opendisplay_i2c.c
  ${SRC_DIR}/opendisplay_sensor_sht40.c
  ${SRC_DIR}/opendisplay_sensor_bq27220.c
  ${SRC_DIR}/opendisplay_sensor_npm1300.c
  ${SRC_DIR}/opendisplay_button.c
  ${SRC_DIR}/opendisplay_config_parser.c
  ${SRC_DIR}/opendisplay_config_storage.c
  ${SRC_DIR}/opendisplay_display_color.c
  ${SRC_DIR}/opendisplay_display.cpp
  ${SRC_DIR}/boot_screen.cpp
  ${SRC_DIR}/opendisplay_epd_map.c
  ${SRC_DIR}/opendisplay_led.c
  ${SRC_DIR}/opendisplay_touch.c
  ${SRC_DIR}/opendisplay_buzzer.c
  ${SRC_DIR}/opendisplay_pipe.c
  ${SRC_DIR}/opendisplay_pipe_write.cpp
  ${SRC_DIR}/opendisplay_cs.c
  ${SRC_DIR}/opendisplay_nfc.c
  ${SRC_DIR}/factory_config.c
  ${GEN_DIR}/factory_config_data.c
  ${SRC_DIR}/qr/qrcode.c
  # OUR glue TU, not the vendored bb_epaper.cpp -- see panel/od_bbep_zephyr.cpp for why.
  ${PANEL_DIR}/od_bbep_zephyr.cpp
  ${THIRD_PARTY_DIR}/bb_epaper/src/Group5.cpp
  ${THIRD_PARTY_DIR}/uzlib/src/od_zlib_stream.c
)

# ------------------------------------------------------------------------------ shared/ ---
# The PURE tier only: these need the C standard library and shared/protocol, nothing else.
# od_adv_control.c and od_watchdog.c are deliberately NOT here -- this target implements
# neither od_hal_adv.h nor od_hal_wdt.h, and taking a source whose HAL is missing buys an
# unreferenced object that links only because nothing calls it. Add each tier when its HAL
# lands, not before.
#
# COMPILED, NOT YET CALLED. src/opendisplay_config_parser.c still owns config parsing here;
# these two are wired in so shared/ is proved to compile under the Zephyr toolchain -- a
# different compiler, different warning set and a freestanding libc from the host tests --
# ahead of the swap that makes them load-bearing. That is the point of doing it now.
target_sources(app PRIVATE ${OD_SHARED_SOURCES_PURE})

# The per-source INCLUDE_DIRECTORIES property that used to sit here is GONE, and its removal is
# the point of the header adoption: it existed only to pin these two shared TUs to the canonical
# opendisplay_protocol.h / opendisplay_structs.h while ${SRC_DIR} still carried subsets of the
# same names. With the subsets deleted there is one definition of each on the include path, so
# the whole target -- shared and local alike -- compiles against the same wire contract, which is
# what the ordering above now enforces for everyone rather than for two files.

if(CONFIG_OD_PLATFORM_NRF52840)
  target_sources(app PRIVATE
    ${SRC_DIR}/platform/nrf52840/od_board_nrf52840.c
    ${SRC_DIR}/platform/nrf52840/od_pin_codec_nrf52840.c)
elseif(CONFIG_SOC_NRF54LM20A)
  target_sources(app PRIVATE
    ${SRC_DIR}/platform/nrf54/od_board_nrf54lm20a.c
    ${SRC_DIR}/platform/nrf54/od_pin_codec_nrf54.c)
elseif(CONFIG_SOC_NRF54L15)
  target_sources(app PRIVATE
    ${SRC_DIR}/platform/nrf54/od_board_nrf54l15.c
    ${SRC_DIR}/platform/nrf54/od_pin_codec_nrf54.c)
endif()

add_dependencies(app factory_config_gen)
