/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * uart_hw — thin wrapper over the RP2350 hardware UART peripheral.
 *
 * Exposes one or more UART instances to the rest of the firmware with
 * two FIFO-backed byte streams: an RX ring fed by the UART IRQ, and a
 * TX ring drained by the UART IRQ. Read/write helpers are non-blocking
 * and return the number of bytes actually moved.
 *
 * Intended consumer: usb/cdc_glue.c (CDC-ACM <-> UART byte shuttle).
 *
 * The scaffold currently instantiates UART0 only — that is the chip's
 * physical-layer bank (RS232/RS485/TTL) on GP0/GP1. UART1 is reserved
 * for the debug console (stdio, 5-pin header) and is managed by the
 * pico-sdk's stdio_uart machinery, not here.
 */

#ifndef RPBRIDGE_UART_HW_H
#define RPBRIDGE_UART_HW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which pico-sdk UART instance — 0 or 1. */
typedef uint8_t uart_hw_instance_t;

enum uart_hw_parity {
    UART_HW_PARITY_NONE = 0,
    UART_HW_PARITY_ODD  = 1,
    UART_HW_PARITY_EVEN = 2,
};

/*
 * Initialise a UART instance with default 115 200 8N1 and attach the
 * given TX/RX GPIOs. The UART FIFOs and the IRQ-driven ring buffers
 * are enabled; bytes start flowing as soon as the upstream peer sends
 * data.
 *
 * Returns 0 on success, -1 on invalid arguments.
 */
int uart_hw_init(uart_hw_instance_t instance, uint8_t tx_pin, uint8_t rx_pin);

/*
 * Reconfigure line coding without tearing the instance down.
 *
 *   baudrate     target baud; the SDK returns the actual achieved rate
 *                which may differ by a fraction of a percent
 *   data_bits    5..8
 *   stop_bits    1 or 2
 *   parity       see uart_hw_parity
 *
 * Returns 0 on success, -1 on invalid arguments (e.g. data_bits=9).
 */
int uart_hw_set_line_coding(uart_hw_instance_t instance,
                            uint32_t baudrate,
                            uint8_t  data_bits,
                            uint8_t  stop_bits,
                            uint8_t  parity);

/*
 * Non-blocking read: drain up to `maxlen` bytes from the RX ring into
 * `buf`. Returns number of bytes actually copied (0 .. maxlen).
 */
size_t uart_hw_read(uart_hw_instance_t instance,
                    uint8_t *buf, size_t maxlen);

/*
 * Non-blocking write: push up to `len` bytes into the TX ring.
 * Returns number of bytes actually queued. Enables the TX-IRQ chain
 * if any bytes were added.
 */
size_t uart_hw_write(uart_hw_instance_t instance,
                     const uint8_t *buf, size_t len);

size_t uart_hw_rx_available(uart_hw_instance_t instance);
size_t uart_hw_tx_space    (uart_hw_instance_t instance);

/*
 * Diagnostics counters. Incremented by the IRQ handler; reading is
 * cheap. Useful for self-test and capability-map stats.
 */
struct uart_hw_stats {
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_overrun;       /* RX ring full, byte dropped */
    uint32_t rx_framing_err;   /* UART framing errors reported by the SDK */
    uint32_t rx_parity_err;
    uint32_t rx_break;
};

const struct uart_hw_stats *uart_hw_stats(uart_hw_instance_t instance);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_UART_HW_H */
