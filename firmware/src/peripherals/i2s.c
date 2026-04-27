/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * i2s — Philips I²S transmitter on PIO2 SM 2.
 *
 * The SM clock is set to 64 × sample_rate_hz so each stereo frame
 * (32 BCLKs × 2 cycles per BCLK) takes exactly 1 / sample_rate_hz
 * seconds. Pixel-rate jitter is bounded by clkdiv resolution
 * (fractional, 16.8 fixed point) — at 48 kHz the SM clock is
 * 3.072 MHz and the divider is ~48.83 so the actual rate is within
 * a few ppm of the request.
 */

#include "i2s.h"

#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include "pico.h"

#if defined(RPBRIDGE_HAVE_I2S) && RPBRIDGE_HAVE_I2S

#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "i2s_tx.pio.h"

#include <string.h>

#ifndef RPBRIDGE_I2S_BCLK_PIN
#  error "RPBRIDGE_I2S_BCLK_PIN must be defined by the active board header"
#endif
#ifndef RPBRIDGE_I2S_LRCLK_PIN
#  error "RPBRIDGE_I2S_LRCLK_PIN must be defined by the active board header"
#endif
#ifndef RPBRIDGE_I2S_DOUT_PIN
#  error "RPBRIDGE_I2S_DOUT_PIN must be defined by the active board header"
#endif

/* The PIO program assumes LRCLK is on BCLK + 1 — enforce at build time. */
_Static_assert(RPBRIDGE_I2S_LRCLK_PIN == RPBRIDGE_I2S_BCLK_PIN + 1u,
               "I2S LRCLK pin must be BCLK + 1 for side-set");

#define I2S_PIO         pio2

struct i2s_state {
    uint     sm;
    uint     program_offset;
    bool     initialised;
    bool     running;
    uint32_t sample_rate_hz;
};

static struct i2s_state g_i2s;

int i2s_init(void)
{
    if (g_i2s.initialised) return 0;

    if (pin_broker_claim(RPBRIDGE_I2S_BCLK_PIN,
                         PIN_OWNER_SYSTEM, PIN_FUNC_PIO) < 0) return -RPBP_EBUSY;
    if (pin_broker_claim(RPBRIDGE_I2S_LRCLK_PIN,
                         PIN_OWNER_SYSTEM, PIN_FUNC_PIO) < 0) {
        pin_broker_release(RPBRIDGE_I2S_BCLK_PIN, PIN_OWNER_SYSTEM);
        return -RPBP_EBUSY;
    }
    if (pin_broker_claim(RPBRIDGE_I2S_DOUT_PIN,
                         PIN_OWNER_SYSTEM, PIN_FUNC_PIO) < 0) {
        pin_broker_release(RPBRIDGE_I2S_BCLK_PIN, PIN_OWNER_SYSTEM);
        pin_broker_release(RPBRIDGE_I2S_LRCLK_PIN, PIN_OWNER_SYSTEM);
        return -RPBP_EBUSY;
    }

    PIO pio = I2S_PIO;
    if (!pio_can_add_program(pio, &rpbridge_i2s_tx_program)) {
        return -RPBP_EBUSY;
    }
    g_i2s.program_offset = pio_add_program(pio, &rpbridge_i2s_tx_program);

    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) return -RPBP_EBUSY;
    g_i2s.sm = (uint)sm;

    g_i2s.sample_rate_hz = 48000u;
    rpbridge_i2s_tx_program_init(pio, g_i2s.sm,
                                 g_i2s.program_offset,
                                 RPBRIDGE_I2S_DOUT_PIN,
                                 RPBRIDGE_I2S_BCLK_PIN,
                                 g_i2s.sample_rate_hz);
    /* Park stopped — host must explicitly i2s_start() to clock out
     * the FIFO. The SM was enabled inside program_init, so disable
     * here. */
    pio_sm_set_enabled(pio, g_i2s.sm, false);

    g_i2s.initialised = true;
    return 0;
}

int i2s_configure(uint32_t sample_rate_hz)
{
    if (!g_i2s.initialised) return -RPBP_EAGAIN;
    if (sample_rate_hz < RPBRIDGE_I2S_MIN_RATE
     || sample_rate_hz > RPBRIDGE_I2S_MAX_RATE) {
        return -RPBP_EINVAL;
    }
    /* Stop, drain, reconfigure, leave stopped. The host must
     * explicitly start again — protects against a half-played
     * buffer transitioning to the new rate mid-frame. */
    if (g_i2s.running) {
        pio_sm_set_enabled(I2S_PIO, g_i2s.sm, false);
        pio_sm_clear_fifos(I2S_PIO, g_i2s.sm);
        g_i2s.running = false;
    }
    pio_sm_set_clkdiv(I2S_PIO, g_i2s.sm,
                      rpbridge_i2s_tx_compute_clkdiv(sample_rate_hz));
    pio_sm_clkdiv_restart(I2S_PIO, g_i2s.sm);
    g_i2s.sample_rate_hz = sample_rate_hz;
    return 0;
}

int i2s_start(void)
{
    if (!g_i2s.initialised) return -RPBP_EAGAIN;
    if (g_i2s.running)      return 0;
    pio_sm_set_enabled(I2S_PIO, g_i2s.sm, true);
    g_i2s.running = true;
    return 0;
}

int i2s_stop(void)
{
    if (!g_i2s.initialised) return -RPBP_EAGAIN;
    if (!g_i2s.running)     return 0;
    /* Drain the FIFO so playback ends at a frame boundary; then
     * disable the SM. */
    while (!pio_sm_is_tx_fifo_empty(I2S_PIO, g_i2s.sm)) {
        tight_loop_contents();
    }
    pio_sm_set_enabled(I2S_PIO, g_i2s.sm, false);
    g_i2s.running = false;
    return 0;
}

int i2s_write(const uint8_t *frames, size_t len)
{
    if (!g_i2s.initialised)        return -RPBP_EAGAIN;
    if (frames == NULL || len == 0u) return -RPBP_EINVAL;
    if ((len & 0x3u) != 0u)        return -RPBP_EINVAL; /* must be 4-byte stereo */

    size_t n_frames = len / 4u;
    for (size_t i = 0; i < n_frames; ++i) {
        /* Host wire layout: L LE16 then R LE16 — i.e. bytes
         *   [L_lo, L_hi, R_lo, R_hi].
         * PIO wants 32-bit word with L in the top 16 bits (MSB
         * first), R in the bottom 16 bits. Build it explicitly so
         * endian-host weirdness can't sneak in. */
        const uint8_t *p = &frames[i * 4u];
        int16_t l = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        int16_t r = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        uint32_t w = ((uint32_t)(uint16_t)l << 16) | (uint32_t)(uint16_t)r;
        pio_sm_put_blocking(I2S_PIO, g_i2s.sm, w);
    }
    return 0;
}

void i2s_get_info(struct i2s_info *out)
{
    if (out == NULL) return;
    out->count          = g_i2s.initialised ? 1u : 0u;
    out->bclk_pin       = RPBRIDGE_I2S_BCLK_PIN;
    out->lrclk_pin      = RPBRIDGE_I2S_LRCLK_PIN;
    out->dout_pin       = RPBRIDGE_I2S_DOUT_PIN;
    out->sample_rate_hz = g_i2s.sample_rate_hz;
    out->bit_depth      = 16u;
    out->running        = g_i2s.running ? 1u : 0u;
    out->reserved[0]    = 0u;
    out->reserved[1]    = 0u;
}

#else  /* !RPBRIDGE_HAVE_I2S */

int  i2s_init(void)                                 { return -RPBP_ENOTSUP; }
int  i2s_configure(uint32_t r)                      { (void)r; return -RPBP_ENOTSUP; }
int  i2s_start(void)                                { return -RPBP_ENOTSUP; }
int  i2s_stop (void)                                { return -RPBP_ENOTSUP; }
int  i2s_write(const uint8_t *f, size_t l)          { (void)f; (void)l; return -RPBP_ENOTSUP; }
void i2s_get_info(struct i2s_info *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
}

#endif /* RPBRIDGE_HAVE_I2S */
