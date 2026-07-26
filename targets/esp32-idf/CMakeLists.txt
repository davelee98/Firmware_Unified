# ESP-IDF project for the OpenDisplay ESP32 targets.
#
# Phase B of the import (docs/MIGRATION.md § "The ESP32 import is different"). The goal of this
# phase is to LINK AND BOOT ON HARDWARE EARLY, so that every later step -- each subsystem
# promoted into shared/core -- is bisectable against a known-good baseline. It is not to be
# clean; cleanliness is phase C's job, enforced by compat/ratchet.sh.
#
#   ./build.sh <board>          e.g. ./build.sh s3-n16r8
#
# Boards are CMake fragments under boards/, not PlatformIO envs. The knob-by-knob translation
# from the source repo's platformio.ini is in ../../docs/TOOLCHAINS.md.

cmake_minimum_required(VERSION 3.16)

# OD_BOARD selects the board fragment. build.sh sets it; a bare `idf.py build` needs
# -DOD_BOARD=<board>, and failing loudly beats silently building a default nobody asked for.
if(NOT DEFINED OD_BOARD)
    message(FATAL_ERROR
        "OD_BOARD is not set. Use ./build.sh <board>, or pass -DOD_BOARD=<board>.\n"
        "Available: see boards/*.cmake")
endif()

set(OD_BOARD_FILE "${CMAKE_CURRENT_LIST_DIR}/boards/${OD_BOARD}.cmake")
if(NOT EXISTS "${OD_BOARD_FILE}")
    message(FATAL_ERROR "Unknown board '${OD_BOARD}' -- no ${OD_BOARD_FILE}")
endif()

# The fragment sets OD_IDF_TARGET, OD_BOARD_DEFINES, OD_PARTITION_CSV and any
# OD_SDKCONFIG_FRAGMENTS before the project() call, because IDF needs the target early.
include("${OD_BOARD_FILE}")

set(IDF_TARGET ${OD_IDF_TARGET})

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
foreach(frag ${OD_SDKCONFIG_FRAGMENTS})
    set(SDKCONFIG_DEFAULTS "${SDKCONFIG_DEFAULTS};${CMAKE_CURRENT_LIST_DIR}/${frag}")
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(opendisplay)
