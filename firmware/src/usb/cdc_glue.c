/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * CDC-ACM <-> UART byte shuttle + TinyUSB class callbacks.
 *
 * ─── Dedicated-CDC-per-phy layout ────────────────────────────────────
 *
 *   CDC #0  "rpbridge UART 1 (RS232)"   ─┐
 *   CDC #1  "rpbridge UART 1 (RS485)"   ─┼─> UART0 HW (only one active)
 *   CDC #2  "rpbridge UART 1 (TTL)"     ─┘
 *
 *   CDC #3  "rpbridge UART 2 (TTL)"      ─> PIO-UART #2
 *
 * With four host-visible virtual COM ports (plus stable udev symlinks
 * on Linux), a developer picks the physical layer by **opening the
 * correct port in any standard terminal** — no custom driver, no
 * control tool. The firmware watches DTR on the three UART0 CDCs and
 * switches the phy-bank transceiver (MAX3232 / SN65HVD72 / direct
 * TTL) to match whichever CDC the host has asserted DTR on.
 *
 * ─── Mutual exclusion ────────────────────────────────────────────────
 *
 * Only one UART0 CDC may own UART0 HW at a time. The most recently
 * DTR-asserted CDC wins; earlier-active CDCs go dormant and have any
 * pending host writes silently discarded so the CDC RX FIFO does not
 * back up and stall the USB pipe. When no CDC has DTR, the phy
 * relaxes to OFF so the transceivers release their outputs.
 *
 * ─── Back-pressure invariants (unchanged from M4a) ───────────────────
 *
 * A pump never reads from a source FIFO unless there is matching
 * room in the destination FIFO / ring. USB-side back-pressure
 * travels through TinyUSB's NAK mechanism; UART-side back-pressure
 * travels through the uart_hw / uart_pio ring-capacity checks.
 *
 * ─── Optional explicit override ──────────────────────────────────────
 *
 * RPBP SET_PHY (`docs/protocol/subsystems/uart.md`) remains as an
 * explicit control path. When invoked, it takes effect immediately
 * and persists until the next DTR transition — at which point the
 * "CDC matches its phy" invariant is restored. This is the
 * driver-dependent fallback path for scripts and for terminals
 * that do not assert DTR.
 */

#include "cdc_glue.h"
#include "peripherals/uart_hw.h"
#include "peripherals/uart_pio.h"
#include "peripherals/uart_phy.h"
#include "commands/cmd_sys.h"

#include "tusb.h"
#include "hardware/uart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ───── CDC index layout ─────────────────────────────────────────────── */

/* CDC slot indices, computed from the board's HAVE_* flags so they
 * mirror the descriptor in usb_descriptors.c. TUSB numbers CDCs in
 * descriptor order starting at 0 — drop a transceiver bank in the
 * board header and the survivors slide down automatically.
 *
 * Each CDC_ITF_* macro is *only defined* when its underlying hardware
 * is present; downstream switch/case + loop bodies must guard their
 * uses with the matching #if RPBRIDGE_HAVE_*. */
#if RPBRIDGE_HAVE_RS232_PHY
#  define CDC_ITF_UART1_RS232    0u
#  define _RPB_CDC_AFTER_RS232   1u
#else
#  define _RPB_CDC_AFTER_RS232   0u
#endif

#if RPBRIDGE_HAVE_RS485_PHY
#  define CDC_ITF_UART1_RS485    (_RPB_CDC_AFTER_RS232)
#  define _RPB_CDC_AFTER_RS485   (_RPB_CDC_AFTER_RS232 + 1u)
#else
#  define _RPB_CDC_AFTER_RS485   _RPB_CDC_AFTER_RS232
#endif

/* TTL is unconditional — UART0 GPIOs are always exposed. */
#define CDC_ITF_UART1_TTL        (_RPB_CDC_AFTER_RS485)

#if RPBRIDGE_HAVE_PIO_UART
#  define CDC_ITF_UART2_TTL      (_RPB_CDC_AFTER_RS485 + 1u)
#endif

/* Which of CDC 0..2 currently owns UART0 HW. -1 = none (phy = OFF). */
#define NO_ACTIVE_UART0_CDC    (-1)
static int g_active_uart0_cdc = NO_ACTIVE_UART0_CDC;

/* Phy-bank UART0 pins from the active board header. */
#define UART0_TX_PIN            RPBRIDGE_UART0_TX_PIN
#define UART0_RX_PIN            RPBRIDGE_UART0_RX_PIN

/* PIO-UART pins from the active board header. Conditional on
 * RPBRIDGE_HAVE_PIO_UART — boards that don't expose a 4th CDC pad
 * the values to 0 and the compile-time test below short-circuits
 * the second uart_pio_init call. */
#define PIO_UART_TX_PIN         RPBRIDGE_PIO_UART_TX_PIN
#define PIO_UART_RX_PIN         RPBRIDGE_PIO_UART_RX_PIN

/* Default baud at power-on. SetLineCoding from the host overrides. */
#define UART_DEFAULT_BAUD       115200u

/* On-stack scratch used to shuttle bytes; matches the CDC data-endpoint
 * size so a full USB packet drains in one iteration. */
#define SCRATCH_BYTES           64u

/* ───── Initialisation ───────────────────────────────────────────────── */

void cdc_glue_init(void)
{
    /* UART0 HW + phy-bank. uart_phy_init() starts in OFF state: all
     * transceivers tri-stated (or directly TTL on bare boards where
     * RPBRIDGE_HAVE_RS232_PHY + RPBRIDGE_HAVE_RS485_PHY are 0). */
    uart_hw_init (0u, UART0_TX_PIN, UART0_RX_PIN);
    uart_phy_init();

#if RPBRIDGE_HAVE_PIO_UART
    /* PIO-UART on PIO1 — TTL-only, always on. */
    uart_pio_init(0u, PIO_UART_TX_PIN, PIO_UART_RX_PIN, UART_DEFAULT_BAUD);
#endif
}

/* ───── Helpers ──────────────────────────────────────────────────────── */

static enum uart_phy_mode cdc_to_phy(uint8_t itf)
{
    switch (itf) {
#if RPBRIDGE_HAVE_RS232_PHY
    case CDC_ITF_UART1_RS232: return UART_PHY_RS232;
#endif
#if RPBRIDGE_HAVE_RS485_PHY
    case CDC_ITF_UART1_RS485: return UART_PHY_RS485;
#endif
    case CDC_ITF_UART1_TTL:   return UART_PHY_TTL;
    default:                  return UART_PHY_OFF;
    }
}

/* Drain and discard any pending bytes on an inactive CDC so its RX
 * FIFO does not fill up and NAK the host to a standstill. */
static void drain_inactive_cdc(uint8_t itf)
{
    uint8_t  tmp[SCRATCH_BYTES];
    while (tud_cdc_n_available(itf)) {
        uint32_t n = tud_cdc_n_read(itf, tmp, sizeof(tmp));
        if (n == 0) break;
    }
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
    /* Drain the 4-word PIO FIFOs first so the RX side cannot back up
     * while we service CDC traffic. */
    uart_pio_tick(0u);

    /* SYS_UART_CLAIM check — when the driver has claimed an instance,
     * the pump stops forwarding bytes between its CDCs and the UART
     * hardware entirely. CDC writes are silently drained so the USB
     * pipe does not stall; UART bytes accumulate in uart_hw's ring
     * (the claim holder is responsible for reading them). See
     * docs/protocol/subsystems/sys.md §UART_CLAIM. */
    const bool uart0_claimed = cmd_sys_uart_claimed(0u);
    const bool uart1_claimed = cmd_sys_uart_claimed(1u);

    /* UART0 HW — only the active CDC bidirects; the other UART0 CDCs
     * (RS232 / RS485 banks, when present) just get their RX FIFOs
     * drained into the bit bucket. A claim on UART0 makes every
     * UART0 CDC look "inactive". On bare-board variants with no
     * transceivers, only TTL is enumerated (one iteration). */
    for (uint8_t itf = 0u; itf <= CDC_ITF_UART1_TTL; ++itf) {
        if (!tud_cdc_n_ready(itf)) continue;

        if (uart0_claimed) {
            drain_inactive_cdc(itf);
        } else if ((int)itf == g_active_uart0_cdc) {
            pump_cdc_to_hw(itf, 0u);
            pump_hw_to_cdc(0u, itf);
        } else {
            drain_inactive_cdc(itf);
        }
    }

#if RPBRIDGE_HAVE_PIO_UART
    /* UART 2 (PIO) — independent from UART0, always bidirectional
     * unless claimed by the driver. Gated on RPBRIDGE_HAVE_PIO_UART. */
    if (tud_cdc_n_ready(CDC_ITF_UART2_TTL)) {
        if (uart1_claimed) {
            drain_inactive_cdc(CDC_ITF_UART2_TTL);
        } else {
            pump_cdc_to_pio(CDC_ITF_UART2_TTL, 0u);
            pump_pio_to_cdc(0u, CDC_ITF_UART2_TTL);
        }
    }
#else
    (void)uart1_claimed;
#endif

    /* RS485 DE deassertion + PIO FIFO trailing drain. */
    uart_hw_tick(0u);
    uart_pio_tick(0u);
}

/* ───── TinyUSB CDC callbacks ───────────────────────────────────────── */

/*
 * Line coding maps directly to the UART backend. All three UART0
 * CDCs share UART0 HW, so any of them reconfigures the same
 * hardware — this is intentional, because `stty` on a freshly-opened
 * COM resets the baud and we want the live UART to follow.
 */
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
#if RPBRIDGE_HAVE_RS232_PHY
    case CDC_ITF_UART1_RS232:
#endif
#if RPBRIDGE_HAVE_RS485_PHY
    case CDC_ITF_UART1_RS485:
#endif
    case CDC_ITF_UART1_TTL:
        uart_hw_set_line_coding(0u, p->bit_rate, p->data_bits, stop, parity);
        break;
#if RPBRIDGE_HAVE_PIO_UART
    case CDC_ITF_UART2_TTL: {
        /* PIO-UART is 8N1-only — see peripherals/uart_pio.h header-block.
         * The helper applies the baud, clamps framing, and returns +1
         * when it had to clamp so we can log and count it instead of
         * pretending the request was honoured. */
        int rc = uart_pio_set_line_coding(0u, p->bit_rate,
                                          p->data_bits, stop, parity);
        if (rc == 1) {
            /* One-shot stdio warning per boot — the stats counter is
             * authoritative, this is just a bring-up hint on UART1. */
            static bool warned_clamp;
            if (!warned_clamp) {
                warned_clamp = true;
                printf("[uart-pio] host requested %lu-%u-%u-%u; clamped to "
                       "8N1 (PIO programs are framing-locked)\n",
                       (unsigned long)p->bit_rate,
                       (unsigned)p->data_bits,
                       (unsigned)p->parity,
                       (unsigned)p->stop_bits);
            }
        }
        break;
    }
#endif
    default:
        break;
    }
}

/*
 * Driverless phy auto-selection.
 *
 *   DTR=1 on any UART0 CDC → switch the phy-bank to that CDC's phy,
 *   mark the CDC as the active gateway. Earlier-active CDCs go
 *   dormant (their bytes will be discarded by the pump until the
 *   host toggles their DTR again).
 *
 *   DTR=0 on the active UART0 CDC → no CDC owns UART0, drop the
 *   phy to OFF (transceivers release).
 *
 *   DTR=0 on a non-active CDC → ignored (it was already dormant).
 *
 * CDC #3 (PIO-UART TTL) is not in the mutual-exclusion group; its
 * DTR is informational.
 */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)rts;

    switch (itf) {
#if RPBRIDGE_HAVE_RS232_PHY
    case CDC_ITF_UART1_RS232:
#endif
#if RPBRIDGE_HAVE_RS485_PHY
    case CDC_ITF_UART1_RS485:
#endif
    case CDC_ITF_UART1_TTL:
        if (dtr) {
            uart_phy_set(cdc_to_phy(itf));
            g_active_uart0_cdc = (int)itf;
        } else if (g_active_uart0_cdc == (int)itf) {
            g_active_uart0_cdc = NO_ACTIVE_UART0_CDC;
            uart_phy_set(UART_PHY_OFF);
        }
        break;

#if RPBRIDGE_HAVE_PIO_UART
    case CDC_ITF_UART2_TTL:
        /* PIO-UART has no transceiver to coordinate. Nothing to do. */
        break;
#endif

    default:
        break;
    }
}

/*
 * Break signal. For the UART0 CDCs we forward to the hardware only
 * if the CDC is the currently-active gateway — otherwise a break on
 * a dormant CDC could disturb the real TX line. The PIO-UART break
 * path is a no-op (the PIO program stalls HIGH on idle, so there is
 * no primitive to drive it LOW for a duration).
 */
void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    bool asserted = (duration_ms != 0u);

    switch (itf) {
#if RPBRIDGE_HAVE_RS232_PHY
    case CDC_ITF_UART1_RS232:
#endif
#if RPBRIDGE_HAVE_RS485_PHY
    case CDC_ITF_UART1_RS485:
#endif
    case CDC_ITF_UART1_TTL:
        if (g_active_uart0_cdc == (int)itf) {
            uart_set_break(uart0, asserted);
        }
        break;
#if RPBRIDGE_HAVE_PIO_UART
    case CDC_ITF_UART2_TTL:
        /* PIO UART: break not implemented in this revision. */
        break;
#endif
    default:
        break;
    }
}
