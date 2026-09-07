cmake_minimum_required(VERSION 3.16)
project(opendisplay_nrf51 C ASM)

if(NOT NRF5_SDK_ROOT)
    message(FATAL_ERROR "Set NRF5_SDK_ROOT to Nordic nRF5 SDK 12.3.0")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

execute_process(
    COMMAND git -C ${CMAKE_CURRENT_LIST_DIR}/../.. rev-parse --short=8 HEAD
    OUTPUT_VARIABLE OD_GIT_SHA8
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(LENGTH "${OD_GIT_SHA8}" OD_GIT_SHA8_LENGTH)
if(NOT OD_GIT_SHA8_LENGTH EQUAL 8 OR NOT OD_GIT_SHA8 MATCHES "^[0-9a-fA-F]+$")
    set(OD_GIT_SHA8 "00000000")
endif()

set(SDK_DEVICE ${NRF5_SDK_ROOT}/components/device)
set(SDK_TOOLCHAIN ${NRF5_SDK_ROOT}/components/toolchain)
set(SDK_S130 ${NRF5_SDK_ROOT}/components/softdevice/s130/headers)

foreach(required
        ${SDK_DEVICE}/nrf.h
        ${SDK_TOOLCHAIN}/gcc/gcc_startup_nrf51.s
        ${SDK_TOOLCHAIN}/system_nrf51.c
        ${SDK_S130}/ble.h)
    if(NOT EXISTS ${required})
        message(FATAL_ERROR "SDK 12.3.0 file missing: ${required}")
    endif()
endforeach()

add_executable(opendisplay-nrf51822
    src/main.c
    src/ble_s130.c
    src/slim_protocol.c
    src/att_long_write.c
    config/lt213a_config.c
    panel/lt213a_panel.c
    ${SDK_TOOLCHAIN}/system_nrf51.c
    ${SDK_TOOLCHAIN}/gcc/gcc_startup_nrf51.s)
set_source_files_properties(${SDK_TOOLCHAIN}/gcc/gcc_startup_nrf51.s PROPERTIES
    COMPILE_FLAGS "-x assembler-with-cpp")

target_include_directories(opendisplay-nrf51822 PRIVATE
    include
    ${CMAKE_CURRENT_LIST_DIR}/../../shared/protocol
    ${SDK_DEVICE}
    ${SDK_TOOLCHAIN}
    ${SDK_TOOLCHAIN}/cmsis/include
    ${SDK_S130})

target_compile_definitions(opendisplay-nrf51822 PRIVATE
    NRF51
    S130
    __START=main
    __STARTUP_CLEAR_BSS
    __HEAP_SIZE=0
    __STACK_SIZE=2048
    OD_NRF51_BUILD_SHA8="${OD_GIT_SHA8}")

target_compile_options(opendisplay-nrf51822 PRIVATE
    -mcpu=cortex-m0
    -mthumb
    -mfloat-abi=soft
    -ffunction-sections
    -fdata-sections
    -flto
    -Os
    -Wall
    -Wextra
    # SDK 12.3's SVCALL headers define parameter-named static assembly stubs.
    -Wno-unused-parameter
    -Werror)

target_link_options(opendisplay-nrf51822 PRIVATE
    -mcpu=cortex-m0
    -mthumb
    -mfloat-abi=soft
    -flto
    -nostartfiles
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/opendisplay-nrf51822.map
    -T${CMAKE_CURRENT_LIST_DIR}/linker/nrf51822_s130.ld
    -L${SDK_TOOLCHAIN}/gcc)

set_target_properties(opendisplay-nrf51822 PROPERTIES SUFFIX ".elf")
add_custom_command(TARGET opendisplay-nrf51822 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex
            $<TARGET_FILE:opendisplay-nrf51822>
            ${CMAKE_CURRENT_BINARY_DIR}/opendisplay-nrf51822.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:opendisplay-nrf51822>
            ${CMAKE_CURRENT_BINARY_DIR}/opendisplay-nrf51822.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:opendisplay-nrf51822>
    VERBATIM)
