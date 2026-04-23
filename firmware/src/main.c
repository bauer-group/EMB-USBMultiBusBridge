/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * RPBridge firmware — entry point.
 *
 * Responsibilities covered in this build:
 *   - TinyUSB device init (composite: 3x CDC-ACM + 2x vendor)
 *   - RPBP v1 transport on the first vendor interface
 *      ├─ HELLO handshake
 *      ├─ PING / PONG / TIME_SYNC
 *      ├─ RESET_CHANNEL
 *      └─ CMD_REQUEST SYS subsystem: GET_CAPABILITIES, ECHO,
 *         REBOOT_BOOTSEL, UPTIME, GET_VBUS_MV, SET_LED, SELFTEST,
 *         GET_IDENTITY, RESET
 *   - Status RGB LED state machine with host-override support
 *   - stdio over CDC-ACM #3 (the debug channel), printf-ready
 *
 * Responsibilities still open (see firmware/TODO.md):
 *   - second vendor interface (gs_usb CAN) glue
 *   - CDC-to-UART-phy bridge (uart1, uart2)
 *   - I²C / SPI / GPIO / PWM / ADC subsystem handlers
 *   - core 1 scheduler for streaming pipelines
 *   - MS OS 2.0 descriptors for Windows WinUSB binding
 */

#include "pico/stdlib.h"
#include "pico/time.h"
#include "tusb.h"

#include "version.h"

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_transport.h"
#include "cmd/cmd_sys.h"

/* ──────────────────────────────────────────────────────────────────────
 * Status LED
 *
 * Common-anode RGB on GP22/23/24. Low on a pin = channel ON.
 * Brightness is binary in this build (no PWM yet); gpio_put(pin, level)
 * toggles the colour component.
 * ────────────────────────────────────────────────────────────────────── */

#ifndef RPBRIDGE_LED_R_PIN
#define RPBRIDGE_LED_R_PIN 22
#endif
#ifndef RPBRIDGE_LED_G_PIN
#define RPBRIDGE_LED_G_PIN 23
#endif
#ifndef RPBRIDGE_LED_B_PIN
#define RPBRIDGE_LED_B_PIN 24
#endif

static void led_init(void)
{
    const uint pins[] = {
        RPBRIDGE_LED_R_PIN, RPBRIDGE_LED_G_PIN, RPBRIDGE_LED_B_PIN
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 1);    /* off: high (common anode) */
    }
}

/* Write a binary colour triple: each component 0 or non-zero. */
static void led_apply_rgb_binary(uint8_t r, uint8_t g, uint8_t b)
{
    gpio_put(RPBRIDGE_LED_R_PIN, r ? 0 : 1);
    gpio_put(RPBRIDGE_LED_G_PIN, g ? 0 : 1);
    gpio_put(RPBRIDGE_LED_B_PIN, b ? 0 : 1);
}

/*
 * LED state machine tick. Called every ~10 ms from the main loop.
 * If a host-issued SET_LED override is active, that wins; otherwise
 * we run the default heartbeat (green, 1 Hz, 50 % duty).
 */
static void led_tick(void)
{
    static uint32_t phase_ms = 0;
    phase_ms += 10;
    if (phase_ms >= 10000) phase_ms = 0;

    if (cmd_sys_led_override_active()) {
        /* Get override state via set-but-peek — cmd_sys stores it
         * internally. For simplicity we read back through a tiny
         * accessor; the override struct lives in cmd_sys.c and the
         * two get/set APIs are cmd_sys_set_led + cmd_sys_led_override_active.
         * For mode-driven animation we mirror the mode values documented
         * in docs/protocol/subsystems/sys.md §SET_LED. */
        /*
         * NOTE: The override *state* (r/g/b/mode) is deliberately not
         * exposed via the current public API to keep the surface area
         * small for the scaffold. We render a stand-in pattern — a
         * magenta solid — to make "override active" visually obvious
         * to a developer on the bench. A future revision can lift the
         * struct into the header once the LED driver has PWM.
         */
        led_apply_rgb_binary(1, 0, 1);
        return;
    }

    /* Default: green 1 Hz heartbeat, 50 % duty. */
    bool on = (phase_ms % 1000) < 500;
    led_apply_rgb_binary(0, on ? 1 : 0, 0);
}

/* ──────────────────────────────────────────────────────────────────────
 * Main
 * ────────────────────────────────────────────────────────────────────── */

int main(void)
{
    stdio_init_all();
    led_init();

    tusb_init();
    rpbp_transport_init();

    /* Small delay so the first printf arrives after USB enumeration. */
    sleep_ms(200);
    printf("RPBridge firmware %s (RPBP v%u.%u.%u)\n",
           RPBRIDGE_GIT_DESCRIBE,
           RPBP_VERSION_MAJOR, RPBP_VERSION_MINOR, RPBP_VERSION_PATCH);

    absolute_time_t next_led = make_timeout_time_ms(0);

    for (;;) {
        tud_task();
        rpbp_transport_poll();

        if (time_reached(next_led)) {
            led_tick();
            next_led = make_timeout_time_ms(10);
        }
    }
}
