cmake_minimum_required(VERSION 3.13.1)

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(THIRD_PARTY_DIR ${PROJECT_ROOT}/third_party)
set(SRC_DIR ${PROJECT_ROOT}/src)
set(GEN_DIR ${SRC_DIR}/generated)

list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_ncs.conf)
list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_cs.conf)

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

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opendisplay_nrf54)

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

target_include_directories(app PRIVATE
  ${SRC_DIR}
  ${THIRD_PARTY_DIR}/bb_epaper/src
  ${THIRD_PARTY_DIR}/uzlib/src
  ${GEN_DIR}
)

target_compile_definitions(app PRIVATE
  TARGET_NRF54
  OD_APP_VERSION=0x0100
  OPENDISPLAY_BUILD_ID=\"nrf54\"
  OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=0
)

if(BOARD MATCHES ".*lm20.*")
  target_compile_definitions(app PRIVATE NRF54_BOARD_LM20)
else()
  target_compile_definitions(app PRIVATE NRF54_BOARD_L15)
endif()

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
  ${SRC_DIR}/board_nrf54.c
  ${SRC_DIR}/nrf54_gpio.c
  ${SRC_DIR}/nrf54_zephyr_time.c
  ${SRC_DIR}/newlib_stubs.c
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
  ${THIRD_PARTY_DIR}/bb_epaper/src/bb_epaper.cpp
  ${THIRD_PARTY_DIR}/bb_epaper/src/Group5.cpp
  ${THIRD_PARTY_DIR}/uzlib/src/od_zlib_stream.c
)

add_dependencies(app factory_config_gen)
