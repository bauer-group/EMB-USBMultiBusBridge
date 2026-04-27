/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * led_strip — addressable LED strip driver (WS2812 / SK6812 / WS2811)
 * via a single PIO state machine on PIO2.
 *
 * Supported pixel formats:
 *   GRB  — WS2812 / WS2812B / SK6812-RGB        (3 bytes per pixel)
 *   RGB  — WS2811 strands, SK9822-style framing (3 bytes per pixel)
 *   GRBW — SK6812-RGBW                          (4 bytes per pixel)
 *
 * The driver maintains a 4-byte-per-pixel framebuffer in SRAM and
 * shifts the active byte stream into the PIO TX FIFO 24 bits at a
 * time (32-bit words, top 24 bits used). After each frame the driver
 * holds the line LOW for ≥ 50 µs to latch the pixels into the strip.
 *
 * Pin claim: the configured RPBRIDGE_LED_STRIP_PIN is claimed on the
 * pin broker as PIN_OWNER_SYSTEM / PIN_FUNC_PIO when the SM is brought
 * up (from led_strip_init()). The user can override the count + format
 * at runtime via led_strip_configure().
 */

#ifndef RPBRIDGE_LED_STRIP_H
#define RPBRIDGE_LED_STRIP_H

#include "pico.h"   /* board feature flags */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RPBRIDGE_LED_MAX_PIXELS
/* 256 pixels × 4 bytes = 1 KB framebuffer. Holds a 16×16 RGBW matrix
 * comfortably and stays well under our 4 KB RPBP MTU when written in
 * one shot. */
#define RPBRIDGE_LED_MAX_PIXELS  256u
#endif

enum led_strip_format {
    LED_STRIP_FMT_GRB  = 0,
    LED_STRIP_FMT_RGB  = 1,
    LED_STRIP_FMT_GRBW = 2,
};

struct led_strip_info {
    uint8_t  count;             /* number of strips (always 1 in v1)     */
    uint8_t  pin;               /* GPIO carrying the data line           */
    uint16_t pixel_count;       /* configured pixels                     */
    uint8_t  format;            /* enum led_strip_format                 */
    uint16_t max_pixels;        /* compile-time ceiling                  */
};

/*
 * Bring the strip driver online: claims PIO2 SM 0, loads the WS2812
 * program, claims RPBRIDGE_LED_STRIP_PIN on the broker, sets a default
 * configuration (1 pixel, GRB, all-off). Idempotent; subsequent calls
 * are no-ops.
 *
 * Returns 0 on success, -RPBP_EBUSY on pin/PIO conflict, -RPBP_ENOTSUP
 * if the build does not include LED support.
 */
int led_strip_init(void);

/*
 * Reconfigure pixel count + format. Reallocates the active range of
 * the static framebuffer; pixel_count > RPBRIDGE_LED_MAX_PIXELS is
 * rejected with -RPBP_EINVAL. Implicitly clears the framebuffer.
 */
int led_strip_configure(uint16_t pixel_count, enum led_strip_format format);

/*
 * Write `len` pixel-bytes starting at byte offset `offset` in the
 * framebuffer. The bytes are interpreted in the strip's configured
 * format (so for GRB, the order on the wire is G0 R0 B0 G1 R1 B1 …).
 * After the partial update the entire frame is shifted into the SM
 * and the latch pulse is generated.
 *
 * Returns 0 on success, -RPBP_EINVAL on out-of-range offset/len,
 * -RPBP_EAGAIN if a previous frame is still draining the FIFO.
 */
int led_strip_write(uint16_t offset, const uint8_t *bytes, size_t len);

/* Convenience: clear all pixels and latch. */
int led_strip_clear(void);

/* Snapshot of the current configuration. */
void led_strip_get_info(struct led_strip_info *out);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_LED_STRIP_H */
