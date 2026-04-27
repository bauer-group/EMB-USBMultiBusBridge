/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Optional USB CDC #4 stdio backend.
 *
 * When RPBRIDGE_DEBUG_CDC is defined at compile time (CMake option
 * of the same name), this module registers a pico-sdk stdio_driver_t
 * that routes printf() output to CDC-ACM index 4 ("RPBridge Debug
 * Log") via tud_cdc_n_write().
 *
 * Policy: writes are non-blocking. If the host has not opened the
 * port (tud_cdc_n_connected() == false) or the TinyUSB FIFO cannot
 * accept all bytes immediately, the surplus is dropped. UART1
 * hardware stdio on the 5-pin debug header remains the always-
 * available channel — losing USB-side bytes during disconnect or
 * congestion is acceptable because either the host wasn't there to
 * receive them or it would have starved out the main loop anyway.
 *
 * When RPBRIDGE_DEBUG_CDC is *not* defined, debug_cdc_init() is a
 * no-op. Calling it unconditionally from main.c keeps the call site
 * free of #ifdef noise.
 */

#include "debug_cdc.h"

#ifdef RPBRIDGE_DEBUG_CDC

#include "pico/stdio/driver.h"
#include "tusb.h"

#define RPBRIDGE_DEBUG_CDC_INDEX 4u

static void debug_cdc_out_chars(const char *buf, int length)
{
    if (length <= 0 || !tud_cdc_n_connected(RPBRIDGE_DEBUG_CDC_INDEX)) {
        return;
    }
    /* tud_cdc_n_write returns the number of bytes accepted into the
     * TX FIFO. We do not retry on partial writes — printf() callers
     * are not waiting for USB bandwidth, and dropping the overflow
     * is preferable to stalling the firmware main loop. The flush
     * call below kicks the TX FIFO drain so short bursts do not sit
     * waiting for the next tud_task() tick. */
    (void) tud_cdc_n_write(RPBRIDGE_DEBUG_CDC_INDEX, buf, (uint32_t) length);
    (void) tud_cdc_n_write_flush(RPBRIDGE_DEBUG_CDC_INDEX);
}

static void debug_cdc_out_flush(void)
{
    if (!tud_cdc_n_connected(RPBRIDGE_DEBUG_CDC_INDEX)) {
        return;
    }
    (void) tud_cdc_n_write_flush(RPBRIDGE_DEBUG_CDC_INDEX);
}

static stdio_driver_t debug_cdc_stdio_driver = {
    .out_chars = debug_cdc_out_chars,
    .out_flush = debug_cdc_out_flush,
    .in_chars  = NULL,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = true,
#endif
};

void debug_cdc_init(void)
{
    stdio_set_driver_enabled(&debug_cdc_stdio_driver, true);
}

#else /* !RPBRIDGE_DEBUG_CDC */

void debug_cdc_init(void)
{
    /* Build option disabled — no USB-side stdio backend registered. */
}

#endif
