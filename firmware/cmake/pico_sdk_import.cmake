# SPDX-License-Identifier: BSD-3-Clause
#
# Thin wrapper that includes the pico-sdk's own pico_sdk_init.cmake from
# the vendored submodule. Project code should include THIS file, which
# then delegates into the SDK. The outer CMakeLists sets PICO_SDK_PATH to
# firmware/third_party/pico-sdk before including us.

if(NOT DEFINED PICO_SDK_PATH OR NOT EXISTS "${PICO_SDK_PATH}")
    message(FATAL_ERROR
        "PICO_SDK_PATH is not set or does not exist. Ensure the pico-sdk "
        "submodule at firmware/third_party/pico-sdk is checked out.")
endif()

# pico-sdk 2.0+ renamed pico_sdk_init.cmake vs. pico_sdk_version.cmake —
# we want the former.
if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR
        "pico_sdk_init.cmake not found at '${PICO_SDK_PATH}'. The pinned "
        "submodule tag is 2.2.0; if you see this error after a fresh "
        "clone, run: git submodule update --init --recursive")
endif()

include("${PICO_SDK_PATH}/pico_sdk_init.cmake")
