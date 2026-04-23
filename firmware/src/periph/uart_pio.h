/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * uart_pio — 8N1 UART implemented on two PIO state machines.
 *
 * The public API intentionally mirrors periph/uart_hw.h so that
 * cdc_glue can route CDC-ACM #0 to uart_hw and CDC-ACM #1 to
 * uart_pio with identical byte-shuttle logic.
 *
 * Limitations of the v1 scaffold:
 *   - data bits: 8  (enforced; SetLineCoding callbacks ignore other values)
 *   - parity:    none
 *   - stop bits: 1
 *   - hardware flow control: none
 *
 * Byte streams are buffered in software rings (RX 2 KB, TX 1 KB)
 * and shuttled between the rings and the PIO FIFOs by a periodic
 * tick that must be called from the main loop.
 */

#ifndef RPBRIDGE_UART_PIO_H
#define RPBRIDGE_UART_PIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t uart_pio_instance_t;

/*
 * Initialise PIO-UART instance. Uses PIO1 (PIO0 is reserved for
 * can2040 / CAN) and consumes two state machines on it: one for TX,
 * one for RX. Both programs are loaded into the block's shared
 * instruction memory once per firmware run.
 *
 * Returns 0 on success, -1 if no SM could be claimed or PIO memory
 * is exhausted.
 */
int uart_pio_init(uart_pio_instance_t instance,
                  uint8_t tx_pin, uint8_t rx_pin,
                  uint32_t baudrate);

/*
 * Reconfigure the baud rate by touching both SM clock dividers.
 * data_bits / stop_bits / parity outside 8 / 1 / NONE are silently
 * clamped. Returns 0 on success, -1 if the instance is not
 * initialised or baud is zero.
 */
int uart_pio_set_line_coding(uart_pio_instance_t instance,
                             uint32_t baudrate,
                             uint8_t  data_bits,
                             uint8_t  stop_bits,
                             uint8_t  parity);

/*
 * Non-blocking read from the software RX ring. Drained by tick from
 * the PIO RX FIFO.
 */
size_t uart_pio_read(uart_pio_instance_t instance,
                     uint8_t *buf, size_t maxlen);

/*
 * Non-blocking write: pushes into the software TX ring. Drained by
 * tick into the PIO TX FIFO.
 */
size_t uart_pio_write(uart_pio_instance_t instance,
                      const uint8_t *buf, size_t len);

size_t uart_pio_rx_available(uart_pio_instance_t instance);
size_t uart_pio_tx_space    (uart_pio_instance_t instance);

/*
 * MUST be called from the main loop frequently enough to keep the
 * 4-word PIO RX FIFO from overflowing at the configured baud rate.
 * At 3 Mbaud one byte lands every ~3.3 µs; 25 µs between ticks is
 * the worst-case period we can tolerate. The main loop runs at tens
 * of kHz, so this is comfortably safe.
 */
void uart_pio_tick(uart_pio_instance_t instance);

struct uart_pio_stats {
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_overrun;       /* software ring full, byte dropped */
    uint32_t rx_framing_err;   /* PIO framing error IRQ raised     */
};

const struct uart_pio_stats *uart_pio_stats(uart_pio_instance_t instance);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_UART_PIO_H */
