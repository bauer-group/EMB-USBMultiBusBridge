/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * CDC-ACM <-> UART byte shuttle + TinyUSB class callbacks.
 *
 * Invariants the pump preserves:
 *   - Never reads from the CDC RX FIFO unless there is matching room
 *     in the UART TX ring. This keeps the USB transfer in flight
 *     until the UART can accept the data, so TinyUSB back-pressures
 *     naturally through the USB NAK mechanism if the UART stalls.
 *   - Never reads from the UART RX ring unless the CDC TX FIFO has
 *     matching room. Same reasoning in the other direction.
 *
 * Two independent bridges run in parallel:
 *
 *     CDC #0 <-> UART0 HW (phy-bank: RS232 / RS485 / TTL on GP0/GP1)
 *     CDC #1 <-> PIO-UART  (TTL-only, GP33 / GP36, PIO1 block)
 */

#include "cdc_glue.h"
#include "periph/uart_hw.h"
#include "periph/uart_pio.h"
#include "periph/uart_phy.h"

#include "tusb.h"
#include "hardware/uart.h"

#include <stdint.h>
#include <stddef.h>

/* CDC index → UART backend mapping. */
#define CDC_ITF_UART1           0u
#define CDC_ITF_UART2           1u

/* Phy-bank UART0 pins (hardware-design.md §6): GP0 TX, GP1 RX. */
#define UART0_TX_PIN            0u
#define UART0_RX_PIN            1u

/* PIO UART pins (hardware-design.md §6): GP33 TX, GP36 RX. */
#define PIO_UART_TX_PIN         33u
#define PIO_UART_RX_PIN         36u

/* Default baud at power-on. SetLineCoding from the host overrides. */
#define UART_DEFAULT_BAUD       115200u

/* On-stack scratch used to shuttle bytes; matches the CDC data-
 * endpoint size so a full USB packet drains in one iteration. */
#define SCRATCH_BYTES           64u

/* ───── Initialisation ───────────────────────────────────────────────── */

void cdc_glue_init(void)
{
    /* UART0 HW + phy-bank: the phy module owns GP6/GP7/GP8 and hands
     * GP6 to uart_hw as the DE pin whenever RS485 is selected. */
    uart_hw_init (0u, UART0_TX_PIN, UART0_RX_PIN);
    uart_phy_init();

    /* PIO-UART on PIO1, two state machines, TTL-only. */
    uart_pio_init(0u, PIO_UART_TX_PIN, PIO_UART_RX_PIN, UART_DEFAULT_BAUD);
}

/* ───── Pump — UART0 HW ──────────────────────────────────────────────── */

static void pump_cdc_to_hw(uint8_t cdc_itf, uart_hw_instance_t uart)
{
    while (tud_cdc_n_available(cdc_itf)) {
        size_t room = uart_hw_tx_space(uart);
        if (room == 0) break;

        uint8_t  tmp[SCRATCH_BYTES];
        size_t   take = room < sizeof(tmp) ? room : sizeof(tmp);
        uint32_t got  = tud_cdc_n_read(cdc_itf, tmp, (uint32_t)take);
        if (got == 0) break;

        (void)uart_hw_write(uart, tmp, got);
    }
}

static void pump_hw_to_cdc(uart_hw_instance_t uart, uint8_t cdc_itf)
{
    bool wrote_any = false;
    while (uart_hw_rx_available(uart)) {
        uint32_t cdc_room = tud_cdc_n_write_available(cdc_itf);
        if (cdc_room == 0) break;

        uint8_t tmp[SCRATCH_BYTES];
        size_t  take = cdc_room < sizeof(tmp) ? cdc_room : sizeof(tmp);
        size_t  got  = uart_hw_read(uart, tmp, take);
        if (got == 0) break;

        tud_cdc_n_write(cdc_itf, tmp, (uint32_t)got);
        wrote_any = true;
    }
    if (wrote_any) {
        tud_cdc_n_write_flush(cdc_itf);
    }
}

/* ───── Pump — PIO UART ──────────────────────────────────────────────── */

static void pump_cdc_to_pio(uint8_t cdc_itf, uart_pio_instance_t uart)
{
    while (tud_cdc_n_available(cdc_itf)) {
        size_t room = uart_pio_tx_space(uart);
        if (room == 0) break;

        uint8_t  tmp[SCRATCH_BYTES];
        size_t   take = room < sizeof(tmp) ? room : sizeof(tmp);
        uint32_t got  = tud_cdc_n_read(cdc_itf, tmp, (uint32_t)take);
        if (got == 0) break;

        (void)uart_pio_write(uart, tmp, got);
    }
}

static void pump_pio_to_cdc(uart_pio_instance_t uart, uint8_t cdc_itf)
{
    bool wrote_any = false;
    while (uart_pio_rx_available(uart)) {
        uint32_t cdc_room = tud_cdc_n_write_available(cdc_itf);
        if (cdc_room == 0) break;

        uint8_t tmp[SCRATCH_BYTES];
        size_t  take = cdc_room < sizeof(tmp) ? cdc_room : sizeof(tmp);
        size_t  got  = uart_pio_read(uart, tmp, take);
        if (got == 0) break;

        tud_cdc_n_write(cdc_itf, tmp, (uint32_t)got);
        wrote_any = true;
    }
    if (wrote_any) {
        tud_cdc_n_write_flush(cdc_itf);
    }
}

/* ───── Public pump entrypoint ───────────────────────────────────────── */

void cdc_glue_poll(void)
{
    /* PIO UART runs on a polled tick: always service the hardware
     * FIFOs before and after the CDC shuttle so 4-word PIO FIFOs
     * stay drained at high baud. */
    uart_pio_tick(0u);

    if (tud_cdc_n_ready(CDC_ITF_UART1)) {
        pump_cdc_to_hw(CDC_ITF_UART1, 0u);
        pump_hw_to_cdc(0u, CDC_ITF_UART1);
    }

    if (tud_cdc_n_ready(CDC_ITF_UART2)) {
        pump_cdc_to_pio(CDC_ITF_UART2, 0u);
        pump_pio_to_cdc(0u, CDC_ITF_UART2);
    }

    /* RS485 DE deassertion runs on a BUSY-flag poll (no IRQ). Ticking
     * every loop iteration keeps the off-edge within a few µs of the
     * real end-of-transmission even at 3 Mbaud. */
    uart_hw_tick(0u);

    uart_pio_tick(0u);
}

/* ───── TinyUSB CDC callbacks ───────────────────────────────────────── */

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *p)
{
    if (p == NULL) return;

    uint8_t stop = (p->stop_bits >= 2u) ? 2u : 1u;

    uint8_t parity;
    switch (p->parity) {
    case 1:  parity = UART_HW_PARITY_ODD;  break;
    case 2:  parity = UART_HW_PARITY_EVEN; break;
    default: parity = UART_HW_PARITY_NONE; break;  /* 0 / mark / space */
    }

    switch (itf) {
    case CDC_ITF_UART1:
        uart_hw_set_line_coding(0u,
                                p->bit_rate,
                                p->data_bits,
                                stop,
                                parity);
        break;
    case CDC_ITF_UART2:
        /* PIO-UART is 8N1-only; the set_line_coding helper silently
         * clamps data/stop/parity and only touches the baud divider. */
        uart_pio_set_line_coding(0u,
                                 p->bit_rate,
                                 p->data_bits,
                                 stop,
                                 parity);
        break;
    default:
        break;
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    /* DTR/RTS are not wired to the hardware transceivers in rev-B.
     * The Arduino-style "reset via DTR" trick is deferred until a
     * small state machine lands that debounces simultaneous edges. */
    (void)itf;
    (void)dtr;
    (void)rts;
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    bool asserted = (duration_ms != 0u);
    switch (itf) {
    case CDC_ITF_UART1:
        uart_set_break(uart0, asserted);
        break;
    case CDC_ITF_UART2:
        /* PIO UART doesn't expose a break primitive; the PIO
         * program inherently emits idle-HIGH during stalls. Break
         * support on PIO lands with a future revision. */
        break;
    default:
        break;
    }
}
