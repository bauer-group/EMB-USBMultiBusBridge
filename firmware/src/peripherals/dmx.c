/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * dmx — DMX512-A transmitter on PIO2.
 *
 * Frame structure on the wire:
 *
 *   ┌──── BREAK (≥ 88 µs LOW) ────┬─ MAB (≥ 8 µs HIGH) ─┐
 *   │                             │                     │
 *   ─────────────────────────────────────────────────────[slot0][slot1]…[slotN]
 *   (idle HIGH before)                                    each slot = start LOW
 *                                                         + 8 data bits LSB-first
 *                                                         + 2 stop bits HIGH
 *
 * The PIO program (src/pio/dmx_tx.pio) emits only the slot UART
 * frames at 250 kbaud 8N2. BREAK and MAB are produced here in the C
 * driver by pausing the SM and driving the pin directly.
 *
 * Refresh cadence: a 513-slot frame takes about 23 ms to transmit
 * end-to-end (44 Hz). That matches the USITT minimum and is what
 * most lighting consoles expect.
 */

#include "dmx.h"

#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include "pico.h"

#if defined(RPBRIDGE_HAVE_DMX) && RPBRIDGE_HAVE_DMX

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#include "dmx_tx.pio.h"

#include <string.h>

#ifndef RPBRIDGE_DMX_TX_PIN
#  error "RPBRIDGE_DMX_TX_PIN must be defined by the active board header"
#endif

#define DMX_PIO         pio2
#define DMX_BREAK_MIN_US  88u
#define DMX_MAB_MIN_US     8u

struct dmx_state {
    uint     sm;
    uint     program_offset;
    bool     initialised;
    bool     running;

    uint16_t slot_count;
    uint16_t break_us;
    uint16_t mab_us;

    uint8_t  buf[DMX_UNIVERSE_SIZE];

    /* Throttle: emit one frame per dmx_tick() iteration whenever the
     * elapsed time since the previous BREAK has exceeded the
     * computed minimum. */
    absolute_time_t next_frame_at;
};

static struct dmx_state g_dmx;

/* Drive the configured TX pin LOW or HIGH directly while the SM is
 * paused. The line stays under PIO control; we use the SM's GPIO
 * write port via pio_sm_set_pins_with_mask64() (the 64-bit variant
 * supports the full RP2350 0..47 GPIO range — rev-B's DMX pin lives
 * at GP44, which would overflow the 32-bit mask form). The mask is
 * just the one bit for our pin, so other SMs sharing the block
 * (none in v1) are unaffected. */
static void drive_pin(bool high)
{
    const uint64_t mask = (uint64_t)1 << RPBRIDGE_DMX_TX_PIN;
    pio_sm_set_pins_with_mask64(DMX_PIO, g_dmx.sm,
                                high ? mask : (uint64_t)0,
                                mask);
}

static void emit_break_mab(void)
{
    /* Drain anything left in the FIFO from a previous frame so we
     * don't emit a partial slot during the BREAK window. */
    pio_sm_set_enabled(DMX_PIO, g_dmx.sm, false);
    pio_sm_clear_fifos(DMX_PIO, g_dmx.sm);
    pio_sm_restart    (DMX_PIO, g_dmx.sm);

    /* BREAK: line LOW for break_us. */
    drive_pin(false);
    sleep_us(g_dmx.break_us);

    /* MAB: line HIGH for mab_us. */
    drive_pin(true);
    sleep_us(g_dmx.mab_us);

    /* Re-enable the SM. The pull-stall keeps the line HIGH until the
     * first slot byte is pushed below. */
    pio_sm_set_enabled(DMX_PIO, g_dmx.sm, true);
}

static void emit_frame(void)
{
    emit_break_mab();
    /* Slots: start code (slot 0) + 1..slot_count channel bytes. The
     * `pio_sm_put_blocking` waits if the FIFO is full so we never
     * drop a slot — total time ~ (slot_count + 1) × 44 µs. */
    uint16_t total = (uint16_t)(g_dmx.slot_count + 1u);   /* +1 = start code */
    for (uint16_t i = 0; i < total; ++i) {
        pio_sm_put_blocking(DMX_PIO, g_dmx.sm, (uint32_t)g_dmx.buf[i]);
    }
}

int dmx_init(void)
{
    if (g_dmx.initialised) return 0;

    if (pin_broker_claim(RPBRIDGE_DMX_TX_PIN,
                         PIN_OWNER_SYSTEM, PIN_FUNC_PIO) < 0) {
        return -RPBP_EBUSY;
    }

    PIO pio = DMX_PIO;
    if (!pio_can_add_program(pio, &rpbridge_dmx_tx_program)) {
        pin_broker_release(RPBRIDGE_DMX_TX_PIN, PIN_OWNER_SYSTEM);
        return -RPBP_EBUSY;
    }
    g_dmx.program_offset = pio_add_program(pio, &rpbridge_dmx_tx_program);

    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        pin_broker_release(RPBRIDGE_DMX_TX_PIN, PIN_OWNER_SYSTEM);
        return -RPBP_EBUSY;
    }
    g_dmx.sm = (uint)sm;

    rpbridge_dmx_tx_program_init(pio, g_dmx.sm,
                                 g_dmx.program_offset,
                                 RPBRIDGE_DMX_TX_PIN);

    /* Default: full universe (512 slots), zeroed payload, stopped. */
    g_dmx.slot_count = 512u;
    g_dmx.break_us   = DMX_DEFAULT_BREAK_US;
    g_dmx.mab_us     = DMX_DEFAULT_MAB_US;
    memset(g_dmx.buf, 0, sizeof(g_dmx.buf));
    g_dmx.running       = false;
    g_dmx.next_frame_at = make_timeout_time_ms(0);

    /* SM is enabled but the line is idle HIGH (the pull stalls in
     * the program). Park it stopped until the host calls dmx_start(). */
    pio_sm_set_enabled(DMX_PIO, g_dmx.sm, false);
    drive_pin(true);

    g_dmx.initialised = true;
    return 0;
}

int dmx_set_slots(uint16_t offset, const uint8_t *bytes, size_t len)
{
    if (!g_dmx.initialised) return -RPBP_EAGAIN;
    if (bytes == NULL && len > 0u) return -RPBP_EINVAL;
    if ((size_t)offset > DMX_UNIVERSE_SIZE) return -RPBP_EINVAL;
    if (len > DMX_UNIVERSE_SIZE - offset)   return -RPBP_EINVAL;
    if (len > 0u) memcpy(&g_dmx.buf[offset], bytes, len);
    return 0;
}

int dmx_set_slot_count(uint16_t slot_count)
{
    if (!g_dmx.initialised) return -RPBP_EAGAIN;
    if (slot_count == 0u || slot_count > 512u) return -RPBP_EINVAL;
    if (slot_count > g_dmx.slot_count) {
        /* New slots default to zero. */
        memset(&g_dmx.buf[g_dmx.slot_count + 1u], 0,
               slot_count - g_dmx.slot_count);
    }
    g_dmx.slot_count = slot_count;
    return 0;
}

int dmx_start(void)
{
    if (!g_dmx.initialised) return -RPBP_EAGAIN;
    if (g_dmx.running)      return 0;
    pio_sm_set_enabled(DMX_PIO, g_dmx.sm, true);
    g_dmx.running       = true;
    g_dmx.next_frame_at = make_timeout_time_ms(0);  /* fire on next tick */
    return 0;
}

int dmx_stop(void)
{
    if (!g_dmx.initialised) return -RPBP_EAGAIN;
    if (!g_dmx.running)     return 0;
    pio_sm_set_enabled(DMX_PIO, g_dmx.sm, false);
    drive_pin(true);                /* idle HIGH */
    g_dmx.running = false;
    return 0;
}

int dmx_set_timing(uint16_t break_us, uint16_t mab_us)
{
    if (!g_dmx.initialised) return -RPBP_EAGAIN;
    if (break_us != 0u) {
        if (break_us < DMX_BREAK_MIN_US) break_us = DMX_BREAK_MIN_US;
        g_dmx.break_us = break_us;
    }
    if (mab_us != 0u) {
        if (mab_us < DMX_MAB_MIN_US) mab_us = DMX_MAB_MIN_US;
        g_dmx.mab_us = mab_us;
    }
    return 0;
}

void dmx_get_info(struct dmx_info *out)
{
    if (out == NULL) return;
    out->count      = g_dmx.initialised ? 1u : 0u;
    out->pin        = RPBRIDGE_DMX_TX_PIN;
    out->running    = g_dmx.running ? 1u : 0u;
    out->slot_count = g_dmx.slot_count;
    out->break_us   = g_dmx.break_us;
    out->mab_us     = g_dmx.mab_us;
}

void dmx_tick(void)
{
    if (!g_dmx.initialised || !g_dmx.running) return;
    if (!time_reached(g_dmx.next_frame_at))   return;

    emit_frame();

    /* Schedule next frame: a 513-slot frame at 250 kbaud + BREAK + MAB
     * takes ~23 ms, so 25 ms gives ~40 Hz refresh with a small
     * scheduling margin. The actual cadence will be slightly slower
     * because emit_frame() blocks until the FIFO drains. */
    g_dmx.next_frame_at = make_timeout_time_ms(25);
}

#else  /* !RPBRIDGE_HAVE_DMX — board opted out */

int  dmx_init(void)                                       { return -RPBP_ENOTSUP; }
int  dmx_set_slots(uint16_t o, const uint8_t *b, size_t l){ (void)o; (void)b; (void)l; return -RPBP_ENOTSUP; }
int  dmx_set_slot_count(uint16_t c)                       { (void)c; return -RPBP_ENOTSUP; }
int  dmx_start(void)                                      { return -RPBP_ENOTSUP; }
int  dmx_stop (void)                                      { return -RPBP_ENOTSUP; }
int  dmx_set_timing(uint16_t b, uint16_t m)               { (void)b; (void)m; return -RPBP_ENOTSUP; }
void dmx_tick(void)                                       {}
void dmx_get_info(struct dmx_info *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
}

#endif /* RPBRIDGE_HAVE_DMX */
