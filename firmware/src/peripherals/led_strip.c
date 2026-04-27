/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * led_strip — WS2812 / SK6812 / WS2811 driver on PIO2.
 *
 * Implementation notes
 * ────────────────────
 *   - One PIO state machine. Programmed at 8 MHz (10 cycles/bit ×
 *     0.125 µs = 1.25 µs/bit, the WS2812 nominal rate).
 *   - The framebuffer holds raw bytes in the strip's configured format.
 *     The wire-shift loop packs each 24-bit (or 32-bit for GRBW) pixel
 *     into one 32-bit word, MSB-first, top 24 bits used. The PIO
 *     program autopulls 24 bits per pixel for GRB/RGB/SK6812; GRBW
 *     pushes a full 32-bit word.
 *   - Latch (reset pulse) = idle the line for ≥ 50 µs. We use 80 µs
 *     to give the slowest WS2812 clones margin.
 */

#include "led_strip.h"

#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include "pico.h"

#if defined(RPBRIDGE_HAVE_LED) && RPBRIDGE_HAVE_LED

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#include "ws2812.pio.h"

#include <string.h>

#ifndef RPBRIDGE_LED_STRIP_PIN
#  error "RPBRIDGE_LED_STRIP_PIN must be defined by the active board header"
#endif

/* Latch hold-time after a frame. Spec minimum is 50 µs; 80 µs gives
 * margin for relabeled WS2812 clones with longer reset thresholds. */
#define LED_STRIP_LATCH_US  80u

#define LED_STRIP_PIO       pio2

struct led_strip_state {
    uint     sm;
    uint     program_offset;
    bool     initialised;

    uint16_t pixel_count;
    uint8_t  format;
    uint8_t  bytes_per_pixel;

    /* Framebuffer in raw wire byte order. Sized for the largest
     * supported configuration (RGBW). */
    uint8_t  buf[RPBRIDGE_LED_MAX_PIXELS * 4u];
};

static struct led_strip_state g_strip;

/* Bytes-per-pixel for each supported format. */
static uint8_t bpp_for_format(enum led_strip_format f)
{
    switch (f) {
    case LED_STRIP_FMT_GRB:
    case LED_STRIP_FMT_RGB:  return 3u;
    case LED_STRIP_FMT_GRBW: return 4u;
    }
    return 3u;
}

/* Pack one pixel from the framebuffer into a 24-bit word for the SM
 * (top 24 bits used; the PIO autopull is configured for 24). For GRBW
 * we send a full 32-bit word (the SM only consumes 24, but a future
 * GRBW-aware applet can pick up the W byte from bits 7..0). */
static uint32_t pack_pixel(const uint8_t *p, enum led_strip_format f)
{
    switch (f) {
    case LED_STRIP_FMT_GRB:
        /* p[0]=G, p[1]=R, p[2]=B → wire order G R B. */
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
             | ((uint32_t)p[2] <<  8);
    case LED_STRIP_FMT_RGB:
        /* p[0]=R, p[1]=G, p[2]=B → swap to wire-canonical GRB so
         * an unmodified WS2812 strip lights the right colour. */
        return ((uint32_t)p[1] << 24) | ((uint32_t)p[0] << 16)
             | ((uint32_t)p[2] <<  8);
    case LED_STRIP_FMT_GRBW:
        /* p[0]=G, p[1]=R, p[2]=B, p[3]=W → 32-bit word.
         * v1 PIO sends only the top 24 bits; full GRBW support is a
         * future SM upgrade. The W byte stays in the framebuffer for
         * forward compat. */
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
             | ((uint32_t)p[2] <<  8);
    }
    return 0u;
}

/* Stream the entire framebuffer to the PIO TX FIFO. Blocking — at
 * 1.25 µs/bit × 24 bits = 30 µs/pixel, even 256 pixels take only
 * ~7.7 ms which fits inside our 100 ms per-RPBP-request budget. */
static void shift_frame(void)
{
    PIO  pio = LED_STRIP_PIO;
    uint sm  = g_strip.sm;
    uint8_t bpp = g_strip.bytes_per_pixel;

    for (uint16_t i = 0; i < g_strip.pixel_count; ++i) {
        uint32_t w = pack_pixel(&g_strip.buf[i * bpp],
                                (enum led_strip_format)g_strip.format);
        pio_sm_put_blocking(pio, sm, w);
    }

    /* Wait for the FIFO to drain before holding latch. The TX-joined
     * FIFO is 8 deep so the worst-case "data still in flight" after
     * the final put is 8 × 30 µs = 240 µs. Sleeping a fixed 80 µs
     * before the next caller frame would compress two frames inside
     * the spec-required >50 µs reset, so poll until the FIFO is
     * empty first. */
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) {
        tight_loop_contents();
    }

    /* Now the SM is shifting out the last pixel. After it finishes,
     * the program stalls waiting for the next pull, holding the line
     * LOW (side-set on the stalling `out`). That LOW idle is the
     * strip's reset window. Hold for ≥ 50 µs. */
    sleep_us(LED_STRIP_LATCH_US);
}

int led_strip_init(void)
{
    if (g_strip.initialised) return 0;

    if (pin_broker_claim(RPBRIDGE_LED_STRIP_PIN,
                         PIN_OWNER_SYSTEM, PIN_FUNC_PIO) < 0) {
        return -RPBP_EBUSY;
    }

    PIO pio = LED_STRIP_PIO;
    if (!pio_can_add_program(pio, &rpbridge_ws2812_program)) {
        pin_broker_release(RPBRIDGE_LED_STRIP_PIN, PIN_OWNER_SYSTEM);
        return -RPBP_EBUSY;
    }
    g_strip.program_offset = pio_add_program(pio, &rpbridge_ws2812_program);

    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        pin_broker_release(RPBRIDGE_LED_STRIP_PIN, PIN_OWNER_SYSTEM);
        return -RPBP_EBUSY;
    }
    g_strip.sm = (uint)sm;

    rpbridge_ws2812_program_init(pio, g_strip.sm,
                                 g_strip.program_offset,
                                 RPBRIDGE_LED_STRIP_PIN);

    /* Default: 1 pixel, GRB, all-off. */
    g_strip.pixel_count     = 1u;
    g_strip.format          = LED_STRIP_FMT_GRB;
    g_strip.bytes_per_pixel = bpp_for_format(LED_STRIP_FMT_GRB);
    memset(g_strip.buf, 0, sizeof(g_strip.buf));
    shift_frame();

    g_strip.initialised = true;
    return 0;
}

int led_strip_configure(uint16_t pixel_count, enum led_strip_format format)
{
    if (!g_strip.initialised) return -RPBP_EAGAIN;
    if (pixel_count == 0u || pixel_count > RPBRIDGE_LED_MAX_PIXELS) {
        return -RPBP_EINVAL;
    }
    switch (format) {
    case LED_STRIP_FMT_GRB:
    case LED_STRIP_FMT_RGB:
    case LED_STRIP_FMT_GRBW:
        break;
    default:
        return -RPBP_EINVAL;
    }
    g_strip.pixel_count     = pixel_count;
    g_strip.format          = (uint8_t)format;
    g_strip.bytes_per_pixel = bpp_for_format(format);
    memset(g_strip.buf, 0,
           (size_t)g_strip.pixel_count * g_strip.bytes_per_pixel);
    shift_frame();
    return 0;
}

int led_strip_write(uint16_t offset, const uint8_t *bytes, size_t len)
{
    if (!g_strip.initialised) return -RPBP_EAGAIN;
    if (bytes == NULL && len > 0u) return -RPBP_EINVAL;

    size_t total = (size_t)g_strip.pixel_count * g_strip.bytes_per_pixel;
    if ((size_t)offset > total) return -RPBP_EINVAL;
    if (len > total - offset)   return -RPBP_EINVAL;

    if (len > 0u) memcpy(&g_strip.buf[offset], bytes, len);
    shift_frame();
    return 0;
}

int led_strip_clear(void)
{
    if (!g_strip.initialised) return -RPBP_EAGAIN;
    size_t total = (size_t)g_strip.pixel_count * g_strip.bytes_per_pixel;
    memset(g_strip.buf, 0, total);
    shift_frame();
    return 0;
}

void led_strip_get_info(struct led_strip_info *out)
{
    if (out == NULL) return;
    out->count       = g_strip.initialised ? 1u : 0u;
    out->pin         = RPBRIDGE_LED_STRIP_PIN;
    out->pixel_count = g_strip.pixel_count;
    out->format      = g_strip.format;
    out->max_pixels  = RPBRIDGE_LED_MAX_PIXELS;
}

#else  /* !RPBRIDGE_HAVE_LED — board opted out */

int led_strip_init(void)                                       { return -RPBP_ENOTSUP; }
int led_strip_configure(uint16_t c, enum led_strip_format f)   { (void)c; (void)f; return -RPBP_ENOTSUP; }
int led_strip_write(uint16_t o, const uint8_t *b, size_t l)    { (void)o; (void)b; (void)l; return -RPBP_ENOTSUP; }
int led_strip_clear(void)                                      { return -RPBP_ENOTSUP; }
void led_strip_get_info(struct led_strip_info *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

#endif /* RPBRIDGE_HAVE_LED */
