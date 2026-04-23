/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * uart_pio — 8N1-only UART implemented on two PIO state machines.
 *
 * Framing constraint (important — not a v1-scaffold footnote)
 * ──────────────────────────────────────────────────────────
 * The bundled `.pio` programs (uart_pio_tx.pio, uart_pio_rx.pio) are
 * hard-wired to frame bytes as:
 *
 *     1 start bit · 8 data bits · no parity · 1 stop bit  =  "8N1"
 *
 * Any other framing (7-bit, 2 stop bits, parity) would need a
 * different pair of PIO programs — the SM instruction counts and
 * clock-per-bit ratios are baked into the assembly. Extending the
 * driver to additional framings therefore means shipping additional
 * programs and a runtime selector, not just a config flag.
 *
 * For now the driver handles non-8N1 line-coding requests *loudly*,
 * not silently:
 *   - baud is always applied verbatim (the SM clock divider is
 *     framing-independent);
 *   - data_bits / stop_bits / parity that differ from 8 / 1 / NONE
 *     are clamped and `uart_pio_set_line_coding()` returns `+1` so
 *     the caller can surface the mismatch;
 *   - the clamped-request counter in `struct uart_pio_stats` is
 *     bumped for every such request — exposed via capability map
 *     / SELFTEST so the host can detect the divergence;
 *   - the capability map advertises the 8N1-only constraint up-front
 *     via the `"uart.pio.8n1-only"` feature flag and `framing:
 *     ["8n1"]` on the bus entry.
 *
 * The public API intentionally mirrors peripherals/uart_hw.h so that
 * cdc_glue can route CDC-ACM #0..2 to uart_hw and CDC-ACM #3 to
 * uart_pio with identical byte-shuttle logic.
 *
 * Other v1-scaffold limitations:
 *   - hardware flow control: none (RTS/CTS not wired through PIO).
 *   - break / overrun signalling: overruns counted, break not detected.
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

/* Line-coding parity encoding matches uart_hw_set_line_coding: 0=none,
 * 1=odd, 2=even. Values outside this set (mark/space) are treated as
 * "none" by both backends for parity-clamp detection. */
#define UART_PIO_PARITY_NONE  0u
#define UART_PIO_PARITY_ODD   1u
#define UART_PIO_PARITY_EVEN  2u

/*
 * Reconfigure the baud rate by touching both SM clock dividers. The
 * framing parameters (data_bits / stop_bits / parity) are validated
 * but NOT applied — the PIO programs are 8N1-only (see header-block
 * doc above).
 *
 * Returns:
 *     0  success, framing matches 8N1 exactly (baud applied).
 *    +1  success, framing was clamped to 8N1 and the `framing_clamped`
 *        stat counter was incremented (baud still applied — the host's
 *        bytes will still flow, but the framing on the wire differs
 *        from what the host requested). Callers SHOULD surface this
 *        to the host (log, event, counter) instead of silently
 *        treating it as plain success.
 *    -1  invalid arguments (instance out of range, baud == 0, or
 *        instance not initialised).
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
    uint32_t rx_overrun;       /* software ring full, byte dropped         */
    uint32_t rx_framing_err;   /* PIO framing error IRQ raised             */
    uint32_t framing_clamped;  /* host requested non-8N1 via SetLineCoding */
};

const struct uart_pio_stats *uart_pio_stats(uart_pio_instance_t instance);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_UART_PIO_H */
