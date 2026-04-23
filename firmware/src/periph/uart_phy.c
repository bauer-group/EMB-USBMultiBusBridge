/*
 * SPDX-License-Identifier: BSD-3-Clause
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

#include "hardware/gpio.h"

#include <stddef.h>

/* Pin assignment follows hardware-design.md §6. */
#define PIN_RS485_DE     6u     /* to SN65HVD72 DE/RE (GP6, PIO-drivable) */
#define PIN_RS485_EN     7u     /* to SN65HVD72 shutdown pin             */
#define PIN_RS232_SHDN   8u     /* to MAX3232 /SHDN (active-high)        */

static enum uart_phy_mode g_mode = UART_PHY_OFF;
static bool               g_initialised = false;

/* ───── Internal helpers ─────────────────────────────────────────────── */

static void setup_output(uint8_t pin, bool level)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, level);
}

static void drive_all_low(void)
{
    gpio_put(PIN_RS485_EN,   0);
    gpio_put(PIN_RS232_SHDN, 0);
    gpio_put(PIN_RS485_DE,   0);
}

/* ───── Public API ───────────────────────────────────────────────────── */

void uart_phy_init(void)
{
    setup_output(PIN_RS485_DE,   false);
    setup_output(PIN_RS485_EN,   false);
    setup_output(PIN_RS232_SHDN, false);

    /* UART0's DE hook stays disconnected until RS485 is selected. */
    uart_hw_set_de_pin(0, -1);

    g_mode = UART_PHY_OFF;
    g_initialised = true;
}

int uart_phy_set(enum uart_phy_mode mode)
{
    if (!g_initialised) return -1;

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

    case UART_PHY_RS232:
        gpio_put(PIN_RS232_SHDN, 1);
        break;

    case UART_PHY_RS485:
        gpio_put(PIN_RS485_EN, 1);
        /* Hand DE to uart_hw so it can toggle it synchronously
         * around every TX burst — see uart_hw_tick(). */
        uart_hw_set_de_pin(0, (int8_t)PIN_RS485_DE);
        break;

    default:
        /* Unknown mode — restore "off" for safety and tell caller. */
        return -1;
    }

    g_mode = mode;
    return 0;
}

enum uart_phy_mode uart_phy_get(void)
{
    return g_mode;
}
