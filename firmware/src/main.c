/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * RPBridge firmware — entry point.
 *
 * Responsibilities covered in this build:
 *   - TinyUSB device init (composite: 4x CDC-ACM + 2x vendor;
 *                          optional 5th CDC for stdio when
 *                          -DRPBRIDGE_DEBUG_CDC=ON)
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
#include "rpbp/rpbp_stream.h"
#include "commands/cmd_sys.h"
#include "usb/cdc_glue.h"
#include "usb/debug_cdc.h"
#include "peripherals/pin_broker.h"
#include "peripherals/i2c_hw.h"
#include "peripherals/spi.h"
#include "peripherals/gpio.h"
#include "peripherals/pwm.h"
#include "peripherals/adc.h"
#include "peripherals/can.h"
#include "peripherals/led_strip.h"
#include "peripherals/dmx.h"
#include "peripherals/onewire.h"
#include "peripherals/i2s.h"
#include "usb/gs_usb_glue.h"
#include "io_control/io_control.h"

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
 * Status LED (PWM-driven, prioritised state scheduler)
 *
 * Hardware: common-anode RGB on GP22/23/24. Anode tied to +3V3, GPIO
 * sinks current — LOW = channel ON. The PWM driver runs channels
 * inverted (GPIO_OVERRIDE_INVERT) so duty_q16 maps the intuitive way:
 * 0 = off, 65535 = full brightness. PWM frequency = 1 kHz.
 *
 * Scheduler: every ~10 ms led_tick() picks a LED "scene" by priority.
 * Highest priority wins; lower-priority scenes are suppressed until
 * the higher one clears. This gives the host a single visual channel
 * without the risk of a chatty subsystem flickering away an important
 * fault indicator.
 *
 *   Priority (high → low)
 *   ─────────────────────
 *     HOST_OVERRIDE      ← SYS SET_LED (mode 1..4, chosen R/G/B/bright)
 *     WATCHDOG_BOOT      ← first 10 s after a watchdog-caused reboot
 *                          (red double-blink — "something misbehaved")
 *     USB_DETACHED       ← blue breathing  — TinyUSB not mounted yet
 *     IDLE               ← green breathing — fallback, everything fine
 *
 * Each scene renders a Q0.16 triple and hands it to led_apply_rgb(),
 * which applies the perceptual R/G/B calibration.
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

#define RPBRIDGE_LED_PWM_HZ         1000u    /* 1 kHz — no flicker, no whine */
#define RPBRIDGE_WATCHDOG_HINT_MS   10000u   /* watchdog scene timeout       */

/*
 * Per-channel max brightness. Different forward voltages across R/G/B
 * LEDs of a common-anode discrete package give uneven apparent
 * brightness at identical duty; a rough perceptual balance for the
 * rev-B part is ~30 % R / 100 % G / 60 % B at "full white".
 */
#define RPBRIDGE_LED_CAL_R   ((uint16_t)(0xFFFFu * 30u / 100u))
#define RPBRIDGE_LED_CAL_G   ((uint16_t)(0xFFFFu * 100u / 100u))
#define RPBRIDGE_LED_CAL_B   ((uint16_t)(0xFFFFu * 60u / 100u))

/* Latched at boot so the watchdog scene can be shown for a bounded
 * window without re-reading the status flag every tick. */
static bool g_boot_from_watchdog;

static int led_init(void)
{
    /* Invert-polarity is the trick that makes duty_q16 intuitive: when
     * the PWM register counts below the threshold the GPIO is HIGH
     * (common-anode = LED off); inverting the output means the same
     * threshold turns the LED ON. duty=0 → LED off, duty=full → ON. */
    const uint8_t pins[] = {
        RPBRIDGE_LED_R_PIN, RPBRIDGE_LED_G_PIN, RPBRIDGE_LED_B_PIN,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        int rc = pwm_hw_configure_led(pins[i], RPBRIDGE_LED_PWM_HZ, 0u);
        if (rc < 0) return -1;
        gpio_set_outover(pins[i], GPIO_OVERRIDE_INVERT);
    }
    return 0;
}

/* Apply a Q0.16 colour triple, scaled by the per-channel perceptual
 * calibration. Inputs 0..65535 map intuitively to off..full-white. */
static void led_apply_rgb(uint16_t r, uint16_t g, uint16_t b)
{
    uint16_t rc = (uint16_t)(((uint32_t)r * RPBRIDGE_LED_CAL_R) >> 16);
    uint16_t gc = (uint16_t)(((uint32_t)g * RPBRIDGE_LED_CAL_G) >> 16);
    uint16_t bc = (uint16_t)(((uint32_t)b * RPBRIDGE_LED_CAL_B) >> 16);
    pwm_hw_set_duty(RPBRIDGE_LED_R_PIN, rc);
    pwm_hw_set_duty(RPBRIDGE_LED_G_PIN, gc);
    pwm_hw_set_duty(RPBRIDGE_LED_B_PIN, bc);
}

/* ───── Scene renderers ──────────────────────────────────────────────── */

/*
 * Expand an 8-bit channel byte to Q0.16 and scale by a 0..100 %
 * brightness. 8-bit white → 16-bit white (255 * 0x101 = 0xFFFF).
 */
static uint16_t expand_u8_q16(uint8_t v, uint8_t brightness_pct)
{
    uint32_t q16 = (uint32_t)v * 0x101u;                    /* 0..65535 */
    q16 = (q16 * brightness_pct) / 100u;
    return (q16 > 0xFFFFu) ? 0xFFFFu : (uint16_t)q16;
}

/* Triangular envelope: 0 → full → 0 over `period_ms` at `phase_ms`. */
static uint16_t triangle_q16(uint32_t phase_ms, uint32_t period_ms)
{
    uint32_t p    = phase_ms % period_ms;
    uint32_t half = period_ms / 2u;
    uint32_t v    = (p < half) ? (p * 0xFFFFu / half)
                               : ((period_ms - p) * 0xFFFFu / half);
    return (v > 0xFFFFu) ? 0xFFFFu : (uint16_t)v;
}

/* Render the SET_LED override per its mode. */
static void render_host_override(const struct cmd_sys_led_override *ovr,
                                 uint32_t phase_ms)
{
    uint16_t r = expand_u8_q16(ovr->r, ovr->brightness_pct);
    uint16_t g = expand_u8_q16(ovr->g, ovr->brightness_pct);
    uint16_t b = expand_u8_q16(ovr->b, ovr->brightness_pct);

    switch (ovr->mode) {
    case 1: /* solid */
        led_apply_rgb(r, g, b);
        return;
    case 2: /* blink 1 Hz — on 500 ms / off 500 ms */
        if ((phase_ms % 1000u) < 500u) led_apply_rgb(r, g, b);
        else                            led_apply_rgb(0, 0, 0);
        return;
    case 3: { /* pulse 1 Hz — triangular breathing */
        uint32_t v = triangle_q16(phase_ms, 1000u);
        led_apply_rgb((uint16_t)((uint32_t)r * v >> 16),
                      (uint16_t)((uint32_t)g * v >> 16),
                      (uint16_t)((uint32_t)b * v >> 16));
        return;
    }
    case 4: /* fast blink 4 Hz — on 125 ms / off 125 ms */
        if ((phase_ms % 250u) < 125u) led_apply_rgb(r, g, b);
        else                           led_apply_rgb(0, 0, 0);
        return;
    default:
        led_apply_rgb(0, 0, 0);
        return;
    }
}

/* Watchdog-boot scene: red double-blink at 2 Hz — unmistakable on the
 * bench that the previous boot was abnormal. */
static void render_watchdog_boot(uint32_t phase_ms)
{
    uint32_t p = phase_ms % 1000u;         /* 2 Hz outer cadence */
    bool on = (p < 100u) || (p >= 200u && p < 300u);
    led_apply_rgb(on ? 0xFFFFu : 0u, 0u, 0u);
}

/* USB-detached scene: blue breathing 1 Hz. */
static void render_usb_detached(uint32_t phase_ms)
{
    led_apply_rgb(0u, 0u, triangle_q16(phase_ms, 1000u));
}

/* Idle scene: green breathing 1 Hz. */
static void render_idle(uint32_t phase_ms)
{
    led_apply_rgb(0u, triangle_q16(phase_ms, 1000u), 0u);
}

/* ───── Top-level LED tick ───────────────────────────────────────────── */

static void led_tick(void)
{
    /* Use absolute boot time as phase so scene transitions don't reset
     * the animation — a transition from "USB-detached" to "idle" keeps
     * the breathing in phase, which looks smoother than restarting. */
    uint32_t phase_ms = (uint32_t)(time_us_64() / 1000u);

    /* 1. Host SET_LED override — unconditional highest priority. */
    struct cmd_sys_led_override ovr;
    if (cmd_sys_get_led_override(&ovr)) {
        render_host_override(&ovr, phase_ms);
        return;
    }

    /* 2. Watchdog-boot indicator — only for the first 10 s after a
     *    recovered-from-watchdog reboot. */
    if (g_boot_from_watchdog && phase_ms < RPBRIDGE_WATCHDOG_HINT_MS) {
        render_watchdog_boot(phase_ms);
        return;
    }

    /* 3. USB enumeration state. `tud_ready()` is true once the host
     *    has sent SET_CONFIGURATION and the CDC/vendor interfaces are
     *    usable. */
    if (!tud_ready()) {
        render_usb_detached(phase_ms);
        return;
    }

    /* 4. Idle — everything fine, green heartbeat. */
    render_idle(phase_ms);
}

/* ──────────────────────────────────────────────────────────────────────
 * Main
 * ────────────────────────────────────────────────────────────────────── */

int main(void)
{
    /*
     * Boot order: report whether the previous reset was caused by the
     * watchdog (firmware hang) or something benign. The capability map
     * exposes this later via a dedicated SYS field; the LED scheduler
     * latches this into g_boot_from_watchdog and renders a red double-
     * blink for the first 10 s after a watchdog-triggered reboot.
     */
    g_boot_from_watchdog = watchdog_caused_reboot();

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
    /* If the build has RPBRIDGE_DEBUG_CDC=1, register the optional
     * USB-side stdio backend on CDC #4. No-op when the flag is OFF. */
    debug_cdc_init();
    rpbp_transport_init();
    rpbp_stream_init();
    cdc_glue_init();
    i2c_hw_init_all();
    if (spi_hw_init_all() != 0) {
        printf("rpbridge: SPI init failed (pin conflict)\n");
    }
    if (adc_hw_init() != 0) {
        printf("rpbridge: ADC init failed (pin conflict)\n");
    }
    /* CAN: 500 kbit/s default (automotive-friendly). Host can re-tune
     * via gs_usb BITTIMING once SocketCAN brings the interface up. */
    if (can_hw_init(500000u) != 0) {
        printf("rpbridge: CAN init failed (pin conflict or can2040 missing)\n");
    }
    gs_usb_glue_init();

    /* PIO2 applets: WS2812 strip and DMX512 transmitter. Both are
     * gated on board feature flags; if either is disabled the call
     * returns ENOTSUP and we keep going. */
    if (led_strip_init() < 0) {
        printf("rpbridge: LED strip init failed (pin/PIO conflict or disabled)\n");
    }
    if (dmx_init() < 0) {
        printf("rpbridge: DMX init failed (pin/PIO conflict or disabled)\n");
    }
    /* PIO1 applet: 1-Wire master. PIO2 applet: I²S TX. */
    if (onewire_init() < 0) {
        printf("rpbridge: 1-Wire init failed (pin/PIO conflict or disabled)\n");
    }
    if (i2s_init() < 0) {
        printf("rpbridge: I²S init failed (pin/PIO conflict or disabled)\n");
    }

    /* Bring the second core online with the I/O scheduler. Core 1
     * idles today; registered services (ADC streamer, TIME_SYNC
     * emitter) hook in here once they land. */
    io_control_start();

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
           g_boot_from_watchdog ? " [recovered from watchdog reset]" : "");

    absolute_time_t next_led = make_timeout_time_ms(0);

    for (;;) {
        tud_task();
        rpbp_transport_poll();
        cdc_glue_poll();
        gs_usb_glue_poll();
        io_control_poll();
        dmx_tick();             /* emits DMX frames at ~40 Hz when running */
        watchdog_update();

        if (time_reached(next_led)) {
            led_tick();
            next_led = make_timeout_time_ms(10);
        }
    }
}
