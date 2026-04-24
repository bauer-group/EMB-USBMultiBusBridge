/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * RPBridge firmware — entry point.
 *
 * Responsibilities covered in this build:
 *   - TinyUSB device init (composite: 4x CDC-ACM + 2x vendor)
 *   - RPBP v1 transport on the first vendor interface
 *      ├─ HELLO handshake · PING/PONG · TIME_SYNC · RESET_CHANNEL
 *      └─ CMD_REQUEST: SYS (9 ops) + I²C + SPI + GPIO + PWM + ADC + UART
 *   - Per-phy CDC-ACM bridge (RS232 / RS485 / TTL + PIO-UART)
 *   - Status RGB LED driven by PWM (GP22/23/24, 1 kHz, perceptual
 *     calibration) with host-override support
 *   - stdio over UART1 on the 5-pin debug header (GP4/GP5), printf-ready
 *
 * Responsibilities still open (see firmware/TODO.md):
 *   - second vendor interface (gs_usb CAN) glue
 *   - core 1 scheduler for streaming pipelines (ADC IIO / GPIO IRQ)
 */

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "version.h"

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_transport.h"
#include "commands/cmd_sys.h"
#include "usb/cdc_glue.h"
#include "peripherals/pin_broker.h"
#include "peripherals/i2c_hw.h"
#include "peripherals/spi.h"
#include "peripherals/gpio.h"
#include "peripherals/pwm.h"
#include "peripherals/adc.h"

/*
 * Watchdog timeout. Must comfortably exceed any expected host-initiated
 * long operation (flash self-program is the worst case — ~200 ms). We
 * settle on 3 s for the base firmware; individual handlers that need
 * longer budget can call watchdog_update() themselves.
 *
 * The `pause_on_debug=true` flag keeps SWD debugging sessions from
 * auto-resetting the part when a breakpoint is hit.
 */
#define RPBRIDGE_WATCHDOG_TIMEOUT_MS 3000u

/* ──────────────────────────────────────────────────────────────────────
 * Status LED (PWM-driven)
 *
 * Common-anode RGB on GP22/23/24. Because the anode is tied HIGH and
 * the GPIO sinks current, a LOW output turns the channel ON. The PWM
 * driver runs channels inverted so duty_q16 maps the way a user
 * expects: 0 = off, 65535 = full brightness. Frequency = 1 kHz
 * (flicker-free, non-audible).
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

#define RPBRIDGE_LED_PWM_HZ  1000u   /* 1 kHz — no flicker, no audible whine */

/*
 * Per-channel max brightness. Different forward voltages across R/G/B
 * LEDs of a common-anode discrete package give uneven apparent
 * brightness at identical duty; a rough perceptual balance for the
 * rev-B part is ~30 % R / 100 % G / 60 % B at "full white".
 */
#define RPBRIDGE_LED_CAL_R   ((uint16_t)(0xFFFFu * 30u / 100u))
#define RPBRIDGE_LED_CAL_G   ((uint16_t)(0xFFFFu * 100u / 100u))
#define RPBRIDGE_LED_CAL_B   ((uint16_t)(0xFFFFu * 60u / 100u))

static int led_init(void)
{
    /* Invert-polarity is the trick that makes duty_q16 intuitive: when
     * the PWM register counts below the threshold the GPIO is HIGH
     * (common-anode = LED off); inverting the output means the same
     * threshold turns the LED ON. duty=0 → LED off, duty=full → ON. */
    struct {
        uint8_t pin;
        uint16_t cal;
    } ch[] = {
        { RPBRIDGE_LED_R_PIN, RPBRIDGE_LED_CAL_R },
        { RPBRIDGE_LED_G_PIN, RPBRIDGE_LED_CAL_G },
        { RPBRIDGE_LED_B_PIN, RPBRIDGE_LED_CAL_B },
    };
    for (size_t i = 0; i < sizeof(ch) / sizeof(ch[0]); ++i) {
        int rc = pwm_hw_configure_led(ch[i].pin, RPBRIDGE_LED_PWM_HZ, 0u);
        if (rc < 0) return -1;
        /* Invert at the GPIO override so "on" = duty>0. */
        gpio_set_outover(ch[i].pin, GPIO_OVERRIDE_INVERT);
        (void)ch[i].cal;   /* calibration is applied at apply() time */
    }
    return 0;
}

/* Apply a colour triple scaled by per-channel calibration. Inputs are
 * Q0.16 per channel (0..65535). */
static void led_apply_rgb(uint16_t r, uint16_t g, uint16_t b)
{
    uint16_t rc = (uint16_t)(((uint32_t)r * RPBRIDGE_LED_CAL_R) >> 16);
    uint16_t gc = (uint16_t)(((uint32_t)g * RPBRIDGE_LED_CAL_G) >> 16);
    uint16_t bc = (uint16_t)(((uint32_t)b * RPBRIDGE_LED_CAL_B) >> 16);
    pwm_hw_set_duty(RPBRIDGE_LED_R_PIN, rc);
    pwm_hw_set_duty(RPBRIDGE_LED_G_PIN, gc);
    pwm_hw_set_duty(RPBRIDGE_LED_B_PIN, bc);
}

/*
 * LED state machine tick. Called every ~10 ms from the main loop.
 * If a host-issued SET_LED override is active, that wins; otherwise
 * we run the default heartbeat — a smooth green sinusoid at 1 Hz
 * instead of the old binary blink, now that PWM is available.
 */
static void led_tick(void)
{
    static uint32_t phase_ms = 0;
    phase_ms += 10;
    if (phase_ms >= 10000) phase_ms = 0;

    if (cmd_sys_led_override_active()) {
        /* Placeholder: magenta pulse at 2 Hz so "override active" is
         * visually obvious on the bench. Once cmd_sys exposes the
         * full override state (r/g/b/mode) we render it here. */
        uint32_t p = phase_ms % 500u;
        uint16_t v = (p < 250u) ? (uint16_t)(p * 262u)
                                : (uint16_t)((500u - p) * 262u);
        led_apply_rgb(v, 0u, v);
        return;
    }

    /* Default: green breathing at 1 Hz, triangular envelope. */
    uint32_t p = phase_ms % 1000u;
    uint16_t v = (p < 500u) ? (uint16_t)(p * 131u)
                            : (uint16_t)((1000u - p) * 131u);
    led_apply_rgb(0u, v, 0u);
}

/* ──────────────────────────────────────────────────────────────────────
 * Main
 * ────────────────────────────────────────────────────────────────────── */

int main(void)
{
    /*
     * Boot order: report whether the previous reset was caused by the
     * watchdog (firmware hang) or something benign. The capability map
     * exposes this later via a dedicated SYS field; for now we just
     * log it.
     */
    const bool boot_from_watchdog = watchdog_caused_reboot();

    stdio_init_all();

    /* The pin broker must exist before any module claims a GPIO;
     * every *_init() call below touches it either eagerly (I²C, SPI,
     * ADC, LED) or lazily (GPIO, PWM-user). */
    pin_broker_init();

    /* pwm_hw_init() must precede led_init() because the LED uses PWM
     * channels on GP22/23/24. */
    pwm_hw_init();
    gpio_hw_init();

    if (led_init() != 0) {
        /* Pin conflicts this early are firmware bugs — log on the
         * debug UART and continue without the status LED so the rest
         * of the USB stack at least still enumerates. */
        printf("rpbridge: LED init failed (pin conflict)\n");
    }

    tusb_init();
    rpbp_transport_init();
    cdc_glue_init();
    i2c_hw_init_all();
    if (spi_hw_init_all() != 0) {
        printf("rpbridge: SPI init failed (pin conflict)\n");
    }
    if (adc_hw_init() != 0) {
        printf("rpbridge: ADC init failed (pin conflict)\n");
    }

    /*
     * Enable the watchdog only after USB + RPBP are up, so the device
     * stays debuggable even if the USB stack takes unusually long to
     * enumerate. From here on, the main loop MUST feed the dog at
     * least every RPBRIDGE_WATCHDOG_TIMEOUT_MS.
     */
    watchdog_enable(RPBRIDGE_WATCHDOG_TIMEOUT_MS, true);

    /* Brief delay so the first printf arrives after UART is quiet. */
    sleep_ms(50);
    printf("RPBridge firmware %s (RPBP v%u.%u.%u)%s\n",
           RPBRIDGE_GIT_DESCRIBE,
           RPBP_VERSION_MAJOR, RPBP_VERSION_MINOR, RPBP_VERSION_PATCH,
           boot_from_watchdog ? " [recovered from watchdog reset]" : "");

    absolute_time_t next_led = make_timeout_time_ms(0);

    for (;;) {
        tud_task();
        rpbp_transport_poll();
        cdc_glue_poll();
        watchdog_update();

        if (time_reached(next_led)) {
            led_tick();
            next_led = make_timeout_time_ms(10);
        }
    }
}
