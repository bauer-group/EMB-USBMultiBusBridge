# SPDX-License-Identifier: MIT
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

# Richer library that pulls in the dispatch + stream layers. Requires
# a small `host_stubs.c` shim to stand in for pico-sdk + per-subsystem
# command handlers. Only the dispatcher round-trip test links this.
set(_rpbp_dispatch_sources
    "${RPBP_SRC}/rpbp/rpbp_dispatch.c"
    "${RPBP_SRC}/rpbp/rpbp_stream.c"
    "${CMAKE_CURRENT_LIST_DIR}/host_stubs.c"
)
add_library(rpbp_dispatch_host STATIC ${_rpbp_dispatch_sources})
target_link_libraries(rpbp_dispatch_host PUBLIC rpbp_portable)
target_include_directories(rpbp_dispatch_host PUBLIC "${RPBP_SRC}")
target_compile_options(rpbp_dispatch_host PRIVATE
    -Wall -Wextra -Wshadow -Wformat=2
)

foreach(_test IN ITEMS test_crc32c test_framing test_cbor test_golden)
    add_executable(${_test} ${_test}.c)
    target_link_libraries(${_test} PRIVATE rpbp_portable)
    target_include_directories(${_test} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_options(${_test} PRIVATE
        -Wall -Wextra -Wshadow -Wformat=2 -Werror
    )
    add_test(NAME ${_test} COMMAND ${_test})
endforeach()

# Dispatcher round-trip test — links the fuller dispatch+stream lib.
add_executable(test_sys_dispatch test_sys_dispatch.c)
target_link_libraries(test_sys_dispatch PRIVATE rpbp_dispatch_host)
target_include_directories(test_sys_dispatch PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_options(test_sys_dispatch PRIVATE
    -Wall -Wextra -Wshadow -Wformat=2 -Werror
)
add_test(NAME test_sys_dispatch COMMAND test_sys_dispatch)

# gen_golden — utility that writes the byte-frozen wire corpus into
# `firmware/tests/golden/`. Not a test; invoke via `cmake --build
# . --target gen-golden` and commit the resulting .hex files.
add_executable(gen_golden gen_golden.c)
target_link_libraries(gen_golden PRIVATE rpbp_portable)
add_custom_target(gen-golden
    COMMAND gen_golden ${CMAKE_CURRENT_LIST_DIR}/../golden
    DEPENDS gen_golden
    COMMENT "Generating byte-frozen RPBP wire corpus under tests/golden/"
)
