/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * uart_phy — UART0 phy-bank selector.
 *
 * UART0 on GP0/GP1 feeds three transceivers in parallel (MAX3232 for
 * RS232, SN65HVD72 for RS485, a direct TTL header). Only one is
 * allowed to drive the TX node at a time; the others tri-state their
 * outputs via shutdown / enable pins:
 *
 *     Target    GP7 RS485 EN   GP8 RS232 SHDN   TTL header
 *     ────────  ─────────────  ───────────────  ───────────────────
 *     OFF       low            low              tri-state
 *     TTL       low            low              direct TX from MCU
 *     RS232     low            high             isolated
 *     RS485     high           low              isolated
 *
 * "OFF" and "TTL" are electrically equivalent at these pins — the
 * difference is semantic: OFF also expects the caller to have
 * disabled UART0 entirely. This module does not touch the UART data
 * path; that responsibility stays in uart_hw.
 *
 * For RS485, the module also owns GP6 (DE). It hands that pin to
 * uart_hw as the DE driver when RS485 is selected, so uart_hw can
 * assert/deassert it around each TX burst.
 */

#ifndef RPBRIDGE_UART_PHY_H
#define RPBRIDGE_UART_PHY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Must match enum rpbp_uart_phy in rpbp.h — the RPBP UART subsystem
 * forwards these values verbatim. */
enum uart_phy_mode {
    UART_PHY_OFF   = 0,
    UART_PHY_TTL   = 1,
    UART_PHY_RS232 = 2,
    UART_PHY_RS485 = 3,
};

/*
 * One-time init. Configures GP6 (DE), GP7 (RS485 EN) and GP8 (RS232
 * SHDN) as outputs, drives them all low (= OFF / TTL state), and
 * stores the initial mode. Call once during firmware bring-up.
 */
void uart_phy_init(void);

/*
 * Switch to a new phy mode. Returns 0 on success, -1 on invalid mode.
 * Transitions are debounced at the GPIO level by toggling disable
 * before enable — no risk of two transceivers driving the same node
 * during the crossover.
 */
int uart_phy_set(enum uart_phy_mode mode);

/* Return the currently-selected phy mode. */
enum uart_phy_mode uart_phy_get(void);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_UART_PHY_H */
