/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * i2s — I²S TX (Philips I²S, 16-bit signed PCM, stereo) on PIO2.
 *
 * v1 scope: TX only; "play this buffer" semantics. WRITE accepts a
 * chunk of stereo PCM frames and the firmware blocks until the SM
 * has consumed them. Suitable for system sounds, beeps, test tones.
 * Continuous audio (multi-second sustained playback) needs an RPBP
 * STREAM_DATA channel — tracked as a follow-up.
 *
 * Sample rate is configurable from 8 kHz to 96 kHz via
 * `i2s_configure(rate)`. The PIO clkdiv is recomputed; clk_sys is
 * 150 MHz by default which gives integer-friendly dividers across
 * the standard rates.
 *
 * Pin layout:
 *   BCLK  = RPBRIDGE_I2S_BCLK_PIN
 *   LRCLK = RPBRIDGE_I2S_BCLK_PIN + 1   (must be consecutive)
 *   DOUT  = RPBRIDGE_I2S_DOUT_PIN       (independent)
 */

#ifndef RPBRIDGE_I2S_H
#define RPBRIDGE_I2S_H

#include "pico.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RPBRIDGE_I2S_MIN_RATE
#define RPBRIDGE_I2S_MIN_RATE   8000u
#endif
#ifndef RPBRIDGE_I2S_MAX_RATE
#define RPBRIDGE_I2S_MAX_RATE   96000u
#endif

struct i2s_info {
    uint8_t  count;            /* TX universe count (1 in v1)             */
    uint8_t  bclk_pin;
    uint8_t  lrclk_pin;
    uint8_t  dout_pin;
    uint32_t sample_rate_hz;   /* currently configured rate               */
    uint8_t  bit_depth;        /* always 16 in v1                         */
    uint8_t  running;          /* 0 = idle, 1 = streaming the FIFO        */
    uint8_t  reserved[2];
};

/*
 * Bring the I²S TX driver online: claims PIO2 SM 2, loads the
 * i2s_tx program, claims the three pins on the broker. Defaults to
 * 48 kHz 16-bit stereo, SM disabled. Idempotent.
 *
 * Returns 0 on success, -RPBP_EBUSY on pin/PIO conflict,
 * -RPBP_ENOTSUP if the build does not include I²S support.
 */
int i2s_init(void);

/*
 * Reconfigure the sample rate. Recomputes the PIO clkdiv and
 * applies it. Stops the SM if it was running. Returns 0 on success,
 * -RPBP_EINVAL on an out-of-range rate.
 */
int i2s_configure(uint32_t sample_rate_hz);

/* Begin / stop the SM. STOP drains any pending FIFO bytes first so
 * playback ends at a frame boundary. */
int i2s_start(void);
int i2s_stop (void);

/*
 * Push stereo PCM frames into the SM. Each frame is 4 bytes
 * (left u16 LE + right u16 LE). The driver translates to the wire
 * format (left in bits 31..16, right in bits 15..0, MSB-first).
 *
 * Blocking until the FIFO has space — that is, the call returns
 * with the entire buffer queued, not necessarily transmitted.
 *
 * Returns 0 on success, -RPBP_EINVAL if `len` is not a multiple of 4
 * or zero / NULL.
 */
int i2s_write(const uint8_t *frames, size_t len);

void i2s_get_info(struct i2s_info *out);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_I2S_H */
