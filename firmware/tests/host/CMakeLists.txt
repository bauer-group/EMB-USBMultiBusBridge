# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 BAUER GROUP
#
# Host-side unit tests for the pure RPBP modules — no pico-sdk
# dependency. Builds with the system C compiler.
#
# Usage:
#   cmake -S firmware/tests/host -B firmware/tests/host/build
#   cmake --build firmware/tests/host/build
#   ctest --test-dir firmware/tests/host/build --output-on-failure

cmake_minimum_required(VERSION 3.28)
project(rpbridge-host-tests C)

set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

set(RPBP_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")

set(_rpbp_portable_sources
    "${RPBP_SRC}/rpbp/rpbp_framing.c"
    "${RPBP_SRC}/rpbp/rpbp_crc32c.c"
    "${RPBP_SRC}/rpbp/rpbp_cbor.c"
)

add_library(rpbp_portable STATIC ${_rpbp_portable_sources})
target_include_directories(rpbp_portable PUBLIC "${RPBP_SRC}")
target_compile_options(rpbp_portable PRIVATE
    -Wall -Wextra -Wshadow -Wformat=2
    -Wstrict-prototypes -Wmissing-prototypes -Werror
)

foreach(_test IN ITEMS test_crc32c test_framing test_cbor)
    add_executable(${_test} ${_test}.c)
    target_link_libraries(${_test} PRIVATE rpbp_portable)
    target_include_directories(${_test} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_options(${_test} PRIVATE
        -Wall -Wextra -Wshadow -Wformat=2 -Werror
    )
    add_test(NAME ${_test} COMMAND ${_test})
endforeach()
