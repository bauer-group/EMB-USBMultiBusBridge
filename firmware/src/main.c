/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * RPBridge — firmware entry point.
 *
 * Responsibilities in scope for this scaffold:
 *   - bring up stdio via CDC-ACM (USB stdio is enabled in CMakeLists)
 *   - initialize the status-LED GPIOs (single discrete RGB on GP22/23/24)
 *   - initialize the TinyUSB device stack
 *   - enter the main cooperative loop, calling tud_task() and a
 *     10 Hz heartbeat blink
 *
 * Responsibilities NOT yet wired up (see firmware/TODO.md):
 *   - RPBP dispatcher on the vendor bulk endpoint
 *   - core 1 launch + cross-core IPC queues
 *   - per-subsystem handlers (I²C, SPI, GPIO, ADC, PWM)
 *   - pin broker
 *   - can2040 glue
 *   - gs_usb compatibility layer
 *   - capability CBOR emitter
 */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "tusb.h"

#include "version.h"
#include "rpbp/rpbp.h"

/*
 * Keep these numeric literals here until a proper GPIO layer lands —
 * having them in one place makes it easy to audit against the board
 * header in boards/rpbridge_rp2354b.h.
 */
#ifndef RPBRIDGE_LED_R_PIN
#define RPBRIDGE_LED_R_PIN 22
#endif
#ifndef RPBRIDGE_LED_G_PIN
#define RPBRIDGE_LED_G_PIN 23
#endif
#ifndef RPBRIDGE_LED_B_PIN
#define RPBRIDGE_LED_B_PIN 24
#endif

static void status_led_init(void)
{
    const uint pins[] = { RPBRIDGE_LED_R_PIN, RPBRIDGE_LED_G_PIN, RPBRIDGE_LED_B_PIN };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        /* Common-anode wiring: HIGH = off, LOW = on. Start all off. */
        gpio_put(pins[i], 1);
    }
}

static void status_led_green(bool on)
{
    gpio_put(RPBRIDGE_LED_G_PIN, on ? 0 : 1);
}

int main(void)
{
    stdio_init_all();
    status_led_init();

    tusb_init();

    printf("RPBridge firmware %s (RPBP v%u.%u.%u)\n",
           RPBRIDGE_GIT_DESCRIBE,
           RPBP_VERSION_MAJOR, RPBP_VERSION_MINOR, RPBP_VERSION_PATCH);

    absolute_time_t next_blink = make_timeout_time_ms(0);
    bool heartbeat = false;

    for (;;) {
        tud_task();

        if (time_reached(next_blink)) {
            heartbeat = !heartbeat;
            status_led_green(heartbeat);
            next_blink = make_timeout_time_ms(500);
        }

        /* Future: drain RPBP requests from the vendor bulk queue here,
         * or dispatch them on the other core. See firmware/TODO.md. */
    }
}
