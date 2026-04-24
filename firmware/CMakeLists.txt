# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 BAUER GROUP

cmake_minimum_required(VERSION 3.28)

# ──────────────────────────────────────────────────────────────────────────
# pico-sdk integration.
#
# pico-sdk is vendored as a git submodule pinned to tag 2.2.0 at
# firmware/third_party/pico-sdk. We point PICO_SDK_PATH at it so the SDK's
# own import machinery (pico_sdk_import.cmake) resolves locally — no
# environment variable required, no network fetch at configure time.
# ──────────────────────────────────────────────────────────────────────────

if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk"
        CACHE PATH "Path to the pico-sdk source tree (pinned submodule)")
endif()

if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR
        "pico-sdk not found at '${PICO_SDK_PATH}'.\n"
        "Run: git submodule update --init --recursive\n"
        "The expected pinned tag is 2.2.0 (see .gitmodules).")
endif()

set(PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards")

# PICO_BOARD / PICO_PLATFORM MUST be set before pico_sdk_import.cmake is
# included — the SDK reads them during its own include to pick defaults
# for compiler, board header, and TinyUSB BSP path. A late `if(NOT)`
# guard after the include arrives too late: the SDK has already defaulted
# to PICO_BOARD=pico / PICO_PLATFORM=rp2040.
if(NOT PICO_BOARD)
    set(PICO_BOARD rpbridge_rp2354b CACHE STRING "Target board")
endif()
if(NOT PICO_PLATFORM)
    set(PICO_PLATFORM rp2350 CACHE STRING "Target platform")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

# ──────────────────────────────────────────────────────────────────────────
# Project declaration (after pico_sdk_import.cmake — the SDK sets the
# cross-compilation toolchain during its own include).
# ──────────────────────────────────────────────────────────────────────────

project(rpbridge
    VERSION      0.1.0
    DESCRIPTION  "RPBridge — USB Multi-Bus Bridge firmware for RP2354B"
    LANGUAGES    C CXX ASM)

set(CMAKE_C_STANDARD             11)
set(CMAKE_CXX_STANDARD           17)
set(CMAKE_C_EXTENSIONS           ON)   # GNU extensions are used by the SDK
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

# ──────────────────────────────────────────────────────────────────────────
# Version string injection: embedded into the USB iProduct descriptor and
# the CBOR capabilities response so hosts can tell builds apart without a
# separate query.
# ──────────────────────────────────────────────────────────────────────────

execute_process(
    COMMAND git describe --tags --always --dirty --match "v*"
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    OUTPUT_VARIABLE RPBRIDGE_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _rpbridge_git_rc)

if(NOT _rpbridge_git_rc EQUAL 0 OR RPBRIDGE_GIT_DESCRIBE STREQUAL "")
    set(RPBRIDGE_GIT_DESCRIBE "v${PROJECT_VERSION}+unknown")
endif()

configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/src/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
    @ONLY)

# ──────────────────────────────────────────────────────────────────────────
# Sources
# ──────────────────────────────────────────────────────────────────────────

add_executable(rpbridge
    src/main.c

    # RPBP v1 protocol core.
    src/rpbp/rpbp_framing.c
    src/rpbp/rpbp_crc32c.c
    src/rpbp/rpbp_cbor.c
    src/rpbp/rpbp_dispatch.c
    src/rpbp/rpbp_transport.c
    src/rpbp/rpbp_stream.c

    # Capability-map emitter.
    src/capabilities/capabilities.c

    # Subsystem handlers.
    src/commands/cmd_sys.c
    src/commands/cmd_uart.c
    src/commands/cmd_i2c.c
    src/commands/cmd_spi.c
    src/commands/cmd_gpio.c
    src/commands/cmd_pwm.c
    src/commands/cmd_adc.c

    # Peripheral drivers.
    src/peripherals/pin_broker.c
    src/peripherals/uart_hw.c
    src/peripherals/uart_pio.c
    src/peripherals/uart_phy.c
    src/peripherals/i2c_hw.c
    src/peripherals/spi.c
    src/peripherals/gpio.c
    src/peripherals/pwm.c
    src/peripherals/adc.c
    src/peripherals/can.c

    # USB composite descriptor set + BOS / MS OS 2.0 for WinUSB binding,
    # plus the CDC-ACM <-> UART byte-shuttle glue and gs_usb (SocketCAN).
    src/usb/usb_descriptors.c
    src/usb/ms_os_20.c
    src/usb/cdc_glue.c
    src/usb/gs_usb_glue.c

    # Core-1 I/O scheduler + cross-core IPC.
    src/io_control/io_control.c

    # See firmware/TODO.md for remaining modules: I2C / SPI / GPIO /
    # PWM / ADC subsystem handlers, pin broker, CAN (can2040 + gs_usb),
    # core1 scheduler, PIO-based precision RS485 DE.
)

# ──────────────────────────────────────────────────────────────────────────
# PIO programs: pioasm turns each .pio file into a C header with the
# program struct, default config, offsets, and instruction array. The
# headers land in the build tree and are found via an added include
# path below.
# ──────────────────────────────────────────────────────────────────────────

pico_generate_pio_header(rpbridge
    ${CMAKE_CURRENT_LIST_DIR}/src/pio/uart_pio_tx.pio)
pico_generate_pio_header(rpbridge
    ${CMAKE_CURRENT_LIST_DIR}/src/pio/uart_pio_rx.pio)

target_include_directories(rpbridge PRIVATE
    src
    src/usb   # so the SDK's tusb.h can resolve `#include "tusb_config.h"`
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    third_party/can2040/src
)

# can2040 is vendored under third_party/ — compiled as part of the main
# executable. The library does not ship a CMakeLists, so its single
# translation unit is added directly when the submodule is present.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/can2040/src/can2040.c")
    target_sources(rpbridge PRIVATE
        third_party/can2040/src/can2040.c
    )
    target_compile_definitions(rpbridge PRIVATE RPBRIDGE_HAVE_CAN2040=1)
else()
    message(STATUS
        "can2040 submodule not present — building without CAN support. "
        "Run: git submodule update --init firmware/third_party/can2040")
endif()

target_compile_options(rpbridge PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wnull-dereference
    -Wformat=2
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wwrite-strings
    -Werror=implicit-function-declaration
    -Werror=return-type
    -fno-common
    $<$<CONFIG:Release>:-Os>
    $<$<CONFIG:RelWithDebInfo>:-Os -g>
    $<$<CONFIG:Debug>:-Og -g3>
)

target_link_libraries(rpbridge PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id

    hardware_dma
    hardware_pio
    hardware_irq
    hardware_i2c
    hardware_spi
    hardware_uart
    hardware_pwm
    hardware_adc
    hardware_gpio
    hardware_flash
    hardware_watchdog

    tinyusb_device
    tinyusb_board
)

# stdio routes to UART1 on the 5-pin debug header (GP4/GP5). Enabling
# pico's stdio_usb would try to add its own CDC-ACM class instance —
# that conflicts with our hand-built composite descriptor (3 CDCs +
# 2 vendors). When a CDC-based debug log is wanted, the firmware writes
# directly to tud_cdc_n_write() on CDC-ACM index 2 ("rpbridge Debug
# Log") — see src/usb/cdc_glue.c once the CDC glue module lands.
pico_enable_stdio_usb(rpbridge 0)
pico_enable_stdio_uart(rpbridge 1)

pico_add_extra_outputs(rpbridge)

# ──────────────────────────────────────────────────────────────────────────
# Host-side unit tests live in firmware/tests/host/ as a **separate**,
# standalone CMake project that uses the host compiler (not the pico-sdk
# cross-toolchain). This avoids mixing toolchain contexts in one build
# tree. See firmware/tests/host/README.md for the usage pattern.
# ──────────────────────────────────────────────────────────────────────────
