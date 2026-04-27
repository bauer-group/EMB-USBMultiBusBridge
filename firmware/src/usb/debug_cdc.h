/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Optional USB CDC #4 stdio backend for the RPBridge firmware.
 *
 * When the CMake option RPBRIDGE_DEBUG_CDC is ON, debug_cdc_init()
 * registers a pico-sdk stdio_driver_t that routes printf() output to
 * CDC-ACM index 4 ("RPBridge Debug Log") via tud_cdc_n_write(). It
 * runs in addition to (not instead of) UART1-hardware stdio on the
 * 5-pin debug header, so SWD-side debugging never depends on USB
 * enumeration state.
 *
 * When the option is OFF, debug_cdc_init() is a no-op — main.c can
 * call it unconditionally without #ifdef noise at the call site.
 */

#ifndef DEBUG_CDC_H
#define DEBUG_CDC_H

#ifdef __cplusplus
extern "C" {
#endif

void debug_cdc_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_CDC_H */
