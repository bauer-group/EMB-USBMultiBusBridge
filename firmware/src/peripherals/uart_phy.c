/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * UART0 phy-bank selector (RS232 / RS485 / TTL).
 *
 * Transition policy: when switching between two modes that use
 * different transceivers we always go via OFF — all enable pins low —
 * before asserting the new one. This avoids the briefly-both-driving
 * scenario if the scheduler were to tear a single `gpio_put()`
 * sequence, and it keeps the transceivers' internal shutdown logic
 * cleanly sequenced.
 */

#include "uart_phy.h"
#include "uart_hw.h"
#include "pin_broker.h"

#include "pico.h"   /* board header */
#include "hardware/gpio.h"

#include <stddef.h>

/* Pin assignment from the board header — only meaningful when the
 * MAX3232 + SN65HVD72 transceivers are populated (rev-B). On a bare
 * Pi Pico 2 board RPBRIDGE_HAVE_RS232_PHY + RPBRIDGE_HAVE_RS485_PHY
 * are both 0 and this whole module compiles to no-op stubs. */
#define PIN_RS485_DE     RPBRIDGE_RS485_DE_PIN
#define PIN_RS485_EN     RPBRIDGE_RS485_EN_PIN
#define PIN_RS232_SHDN   RPBRIDGE_RS232_SHDN_PIN

#if !RPBRIDGE_HAVE_RS232_PHY && !RPBRIDGE_HAVE_RS485_PHY
/* Bare-board variant: phy selector collapses to TTL-only with all
 * other modes returning ENOTSUP. */
#define UART_PHY_TTL_ONLY 1
#else
#define UART_PHY_TTL_ONLY 0
#endif

static enum uart_phy_mode g_mode = UART_PHY_OFF;
static bool               g_initialised = false;

/* ───── Internal helpers ─────────────────────────────────────────────── */

#if !UART_PHY_TTL_ONLY
static void setup_output(uint8_t pin, bool level)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, level);
}

static void drive_all_low(void)
{
#  if RPBRIDGE_HAVE_RS485_PHY
    gpio_put(PIN_RS485_EN,   0);
    gpio_put(PIN_RS485_DE,   0);
#  endif
#  if RPBRIDGE_HAVE_RS232_PHY
    gpio_put(PIN_RS232_SHDN, 0);
#  endif
}
#endif /* !UART_PHY_TTL_ONLY */

/* ───── Public API ───────────────────────────────────────────────────── */

int uart_phy_init(void)
{
#if UART_PHY_TTL_ONLY
    /* Bare-board variant — no transceivers populated. UART runs in
     * TTL mode by definition; nothing to claim, nothing to drive. */
    g_mode = UART_PHY_TTL;
    g_initialised = true;
    return 0;
#else
    /* All three pads belong to the phy module for their entire
     * life, independent of which mode is active. The pin broker
     * records the ownership so nothing else can silently take one
     * of them for another purpose. */
#  if RPBRIDGE_HAVE_RS485_PHY
    if (pin_broker_claim(PIN_RS485_DE,   PIN_OWNER_UART_PHY, PIN_FUNC_GPIO_OUT) < 0) return -1;
    if (pin_broker_claim(PIN_RS485_EN,   PIN_OWNER_UART_PHY, PIN_FUNC_GPIO_OUT) < 0) return -1;
    setup_output(PIN_RS485_DE,   false);
    setup_output(PIN_RS485_EN,   false);
#  endif
#  if RPBRIDGE_HAVE_RS232_PHY
    if (pin_broker_claim(PIN_RS232_SHDN, PIN_OWNER_UART_PHY, PIN_FUNC_GPIO_OUT) < 0) return -1;
    setup_output(PIN_RS232_SHDN, false);
#  endif

    /* UART0's DE hook stays disconnected until RS485 is selected. */
    uart_hw_set_de_pin(0, -1);

    g_mode = UART_PHY_OFF;
    g_initialised = true;
    return 0;
#endif /* !UART_PHY_TTL_ONLY */
}

int uart_phy_set(enum uart_phy_mode mode)
{
    if (!g_initialised) return -1;

#if UART_PHY_TTL_ONLY
    /* Bare-board: only TTL is meaningful. OFF is treated as TTL too
     * (nothing to switch off). RS232/RS485 requests fail with EINVAL
     * — there is no transceiver to enable. */
    if (mode == UART_PHY_TTL || mode == UART_PHY_OFF) {
        g_mode = UART_PHY_TTL;
        return 0;
    }
    return -1;
#else
    /* Step 1: drive everything low to drop the currently-active
     * transceiver before enabling the next. ~100 ns is enough for
     * the SN65HVD72 and MAX3232 to release their output stages. */
    drive_all_low();
    uart_hw_set_de_pin(0, -1);

    /* Step 2: apply the requested mode. */
    switch (mode) {
    case UART_PHY_OFF:
    case UART_PHY_TTL:
        /* Already all-low; nothing else to do. */
        break;

#  if RPBRIDGE_HAVE_RS232_PHY
    case UART_PHY_RS232:
        gpio_put(PIN_RS232_SHDN, 1);
        break;
#  endif

#  if RPBRIDGE_HAVE_RS485_PHY
    case UART_PHY_RS485:
        gpio_put(PIN_RS485_EN, 1);
        /* Hand DE to uart_hw so it can toggle it synchronously
         * around every TX burst — see uart_hw_tick(). */
        uart_hw_set_de_pin(0, (int8_t)PIN_RS485_DE);
        break;
#  endif

    default:
        /* Mode not supported on this board (transceiver not
         * populated). Caller maps to ENOTSUP. */
        return -1;
    }

    g_mode = mode;
    return 0;
#endif /* !UART_PHY_TTL_ONLY */
}

enum uart_phy_mode uart_phy_get(void)
{
    return g_mode;
}
