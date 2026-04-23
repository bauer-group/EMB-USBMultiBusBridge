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

if(NOT PICO_BOARD)
    set(PICO_BOARD rpbridge_rp2354b CACHE STRING "Target board")
endif()
if(NOT PICO_PLATFORM)
    set(PICO_PLATFORM rp2350 CACHE STRING "Target platform")
endif()

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

    # Capability-map emitter.
    src/cap/capabilities.c

    # SYS subsystem (subsys = 0) handlers.
    src/cmd/cmd_sys.c

    # USB composite descriptor set.
    src/usb/usb_descriptors.c

    # See firmware/TODO.md for remaining modules: per-bus cmd handlers
    # (i2c/spi/gpio/pwm/adc/uart), pin broker, peripheral drivers, CAN
    # glue, core1 scheduler, gs_usb compat layer, MS OS 2.0 descriptors.
)

target_include_directories(rpbridge PRIVATE
    src
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

# stdio over CDC-ACM #3 (Debug Log); stdio over UART disabled so it doesn't
# fight with the user UART lines.
pico_enable_stdio_usb(rpbridge 1)
pico_enable_stdio_uart(rpbridge 0)

pico_add_extra_outputs(rpbridge)

# ──────────────────────────────────────────────────────────────────────────
# Host-side unit tests live in firmware/tests/host/ as a **separate**,
# standalone CMake project that uses the host compiler (not the pico-sdk
# cross-toolchain). This avoids mixing toolchain contexts in one build
# tree. See firmware/tests/host/README.md for the usage pattern.
# ──────────────────────────────────────────────────────────────────────────
