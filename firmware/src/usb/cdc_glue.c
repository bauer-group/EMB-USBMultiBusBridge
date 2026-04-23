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
 */

#include "cdc_glue.h"
#include "periph/uart_hw.h"

#include "tusb.h"
#include "hardware/uart.h"

#include <stdint.h>
#include <stddef.h>

/*
 * CDC index -> UART HW instance mapping. CDC #1 is reserved for the
 * PIO-UART and has no HW instance yet; the pump skips it.
 *
 * Keep pin assignments close to the spec (hardware-design.md §6):
 *   GP0 = UART0 TX  (feeds the phy-bank: RS232/RS485/TTL)
 *   GP1 = UART0 RX
 */
#define CDC_ITF_UART1           0u
#define CDC_ITF_UART2           1u    /* PIO-UART; pump stub for now */

#define UART0_TX_PIN            0u
#define UART0_RX_PIN            1u

/* Size of on-stack scratch used to shuttle bytes; 64 B matches the
 * CDC data-endpoint size, so a full USB packet can be drained in one
 * iteration. */
#define SCRATCH_BYTES           64u

/* ───── Initialisation ───────────────────────────────────────────────── */

void cdc_glue_init(void)
{
    /* Wire CDC #0 to UART0 HW on the phy-bank pins. */
    uart_hw_init(0u, UART0_TX_PIN, UART0_RX_PIN);
    /* CDC #1 -> PIO-UART: nothing to do yet. */
}

/* ───── Pump ─────────────────────────────────────────────────────────── */

static void pump_cdc_to_uart(uint8_t cdc_itf, uart_hw_instance_t uart)
{
    while (tud_cdc_n_available(cdc_itf)) {
        size_t room = uart_hw_tx_space(uart);
        if (room == 0) break;                  /* UART back-pressure */

        uint8_t  tmp[SCRATCH_BYTES];
        size_t   take = room < sizeof(tmp) ? room : sizeof(tmp);
        uint32_t got  = tud_cdc_n_read(cdc_itf, tmp, (uint32_t)take);
        if (got == 0) break;

        /* uart_hw_write() is guaranteed to accept `got` bytes because
         * we already checked `room` and `got <= take <= room`. */
        (void)uart_hw_write(uart, tmp, got);
    }
}

static void pump_uart_to_cdc(uart_hw_instance_t uart, uint8_t cdc_itf)
{
    bool wrote_any = false;

    while (uart_hw_rx_available(uart)) {
        uint32_t cdc_room = tud_cdc_n_write_available(cdc_itf);
        if (cdc_room == 0) break;               /* host back-pressure */

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

void cdc_glue_poll(void)
{
    if (!tud_cdc_n_ready(CDC_ITF_UART1)) {
        /* Host hasn't opened the port yet — skip silently. */
    } else {
        pump_cdc_to_uart(CDC_ITF_UART1, 0u);
        pump_uart_to_cdc(0u, CDC_ITF_UART1);
    }

    /* CDC #1 (PIO-UART) pump lives here once the PIO UART ships. */
}

/* ───── TinyUSB CDC callbacks ───────────────────────────────────────── */

/*
 * Map CDC 1.2 line-coding fields onto the UART HW driver. The CDC
 * parity code set is wider than the UART hardware supports — we
 * silently clamp mark/space down to none, which is the least
 * surprising default for a developer tool.
 */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *p)
{
    if (p == NULL) return;

    uint8_t stop = (p->stop_bits >= 2u) ? 2u : 1u;

    uint8_t parity;
    switch (p->parity) {
    case 1:  parity = UART_HW_PARITY_ODD;  break;
    case 2:  parity = UART_HW_PARITY_EVEN; break;
    default: parity = UART_HW_PARITY_NONE; break;  /* 0/mark/space */
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
        /* PIO-UART line coding pending M4b. */
        break;
    default:
        break;
    }
}

/*
 * DTR/RTS transitions. For the scaffold we acknowledge but don't act
 * on them. The classic Arduino / Pico "DTR=low + RTS=low -> reboot
 * to BOOTSEL" trick lands later — it needs a small state machine to
 * debounce the simultaneous edges the host driver typically produces.
 */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)dtr;
    (void)rts;
}

/*
 * Break signal: the host asks the device to drive the TX line low
 * for either a fixed duration (ms) or, per CDC 1.2, indefinitely
 * (0xFFFF means "start", 0 means "stop"). We pass that directly to
 * the SDK, which sets the UART BREAK bit — host is responsible for
 * clearing it.
 */
void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    bool asserted = (duration_ms != 0u);

    switch (itf) {
    case CDC_ITF_UART1:
        uart_set_break(uart0, asserted);
        break;
    case CDC_ITF_UART2:
        /* PIO-UART break pending M4b. */
        break;
    default:
        break;
    }
}
