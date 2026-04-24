/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * io_control — second-core scheduler + cross-core event pump.
 */

#include "io_control.h"

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"
#include "rpbp/rpbp_stream.h"
#include "peripherals/adc.h"

#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/sync.h"

#include <string.h>

/* ───── Queue endpoints ──────────────────────────────────────────────── */

#define IO_GPIO_Q_SLOTS    64u
_Static_assert((IO_GPIO_Q_SLOTS & (IO_GPIO_Q_SLOTS - 1u)) == 0,
               "queue size must be a power of two");

static struct ipc_queue     g_gpio_q;
static struct io_gpio_event g_gpio_q_storage[IO_GPIO_Q_SLOTS];

struct ipc_queue *io_control_gpio_queue(void)
{
    return &g_gpio_q;
}

/* ───── ADC sampler queue + config ───────────────────────────────────── */

#define IO_ADC_Q_SLOTS      128u
_Static_assert((IO_ADC_Q_SLOTS & (IO_ADC_Q_SLOTS - 1u)) == 0,
               "queue size must be a power of two");

static struct ipc_queue      g_adc_q;
static struct io_adc_sample  g_adc_q_storage[IO_ADC_Q_SLOTS];

struct ipc_queue *io_control_adc_queue(void)
{
    return &g_adc_q;
}

/*
 * Sampler config. Packed into a single u32 so we can update it
 * atomically with a store (Core 0 writes, Core 1 reads).
 *   bits  0.. 7 : channel_mask
 *   bits  8..31 : period_us  (0 = sampler off; capped at 0xFFFFFF us)
 */
static volatile uint32_t g_adc_cfg;

static inline uint32_t pack_adc_cfg(uint8_t mask, uint32_t period_us)
{
    if (period_us > 0xFFFFFFu) period_us = 0xFFFFFFu;
    return ((uint32_t)mask) | (period_us << 8);
}

void io_control_adc_configure(uint8_t channel_mask, uint32_t period_us)
{
    __dmb();
    g_adc_cfg = pack_adc_cfg(channel_mask, period_us);
    __dmb();
}

void io_control_adc_config(uint8_t *channel_mask_out, uint32_t *period_us_out)
{
    uint32_t v = g_adc_cfg;
    if (channel_mask_out) *channel_mask_out = (uint8_t)(v & 0xFFu);
    if (period_us_out)    *period_us_out    = v >> 8;
}

static volatile uint16_t g_adc_stream_channel = 0xFFFFu;

void io_control_adc_stream_channel(uint16_t channel)
{
    g_adc_stream_channel = channel;
}

uint16_t io_control_adc_stream_channel_get(void)
{
    return g_adc_stream_channel;
}

static inline unsigned popcount8(uint8_t v)
{
    v = (uint8_t)(v - ((v >> 1) & 0x55u));
    v = (uint8_t)((v & 0x33u) + ((v >> 2) & 0x33u));
    return (unsigned)((v + (v >> 4)) & 0x0Fu);
}

/* ───── Stats ────────────────────────────────────────────────────────── */

static struct io_control_stats g_stats;

void io_control_stats(struct io_control_stats *out)
{
    if (out != NULL) *out = g_stats;
}

/* ───── Event emission (Core 0 side) ─────────────────────────────────── */

/*
 * Build a RPBP EVENT payload for a GPIO edge and push it onto the
 * events channel.
 *
 * Payload layout (binary — not CBOR):
 *   [0]      subsys   = RPBP_SUBSYS_GPIO (0x03)
 *   [1]      event_id = 0x00  (edge)
 *   [2]      pin
 *   [3]      level
 *   [4..7]   timestamp_us  (LE u32, host clock-synced via TIME_SYNC)
 */
static bool emit_gpio_event(const struct io_gpio_event *ev)
{
    uint8_t payload[8];
    payload[0] = (uint8_t)RPBP_SUBSYS_GPIO;
    payload[1] = 0x00u;                       /* edge event */
    payload[2] = ev->pin;
    payload[3] = ev->level;
    payload[4] = (uint8_t)(ev->timestamp_us         & 0xFFu);
    payload[5] = (uint8_t)((ev->timestamp_us >> 8)  & 0xFFu);
    payload[6] = (uint8_t)((ev->timestamp_us >> 16) & 0xFFu);
    payload[7] = (uint8_t)((ev->timestamp_us >> 24) & 0xFFu);

    return rpbp_send_event(0u, payload, sizeof(payload)) == 0;
}

/*
 * Build a STREAM_DATA payload from a single sample tick and emit it
 * on the configured ADC channel.
 *
 * Payload layout (matches docs/protocol/subsystems/adc.md §stream):
 *   [0..3]         timestamp_us  (LE u32)
 *   [4]            channel_mask
 *   [5]            sample_count   (popcount of mask)
 *   [6..6+2n-1]    samples (LE u16 × sample_count)
 */
static void emit_adc_stream(uint16_t channel, const struct io_adc_sample *s)
{
    uint8_t buf[6 + 2 * 4];
    buf[0] = (uint8_t)(s->timestamp_us         & 0xFFu);
    buf[1] = (uint8_t)((s->timestamp_us >> 8)  & 0xFFu);
    buf[2] = (uint8_t)((s->timestamp_us >> 16) & 0xFFu);
    buf[3] = (uint8_t)((s->timestamp_us >> 24) & 0xFFu);
    buf[4] = s->channel_mask;
    buf[5] = s->sample_count;
    size_t off = 6;
    for (unsigned i = 0; i < s->sample_count && i < 4u; ++i) {
        buf[off++] = (uint8_t)(s->samples[i] & 0xFFu);
        buf[off++] = (uint8_t)((s->samples[i] >> 8) & 0xFFu);
    }
    (void)rpbp_stream_send_frame(channel, buf, off);
}

void io_control_poll(void)
{
    /* Drain up to a bounded number of GPIO events per poll so a
     * misbehaving switch-bounce storm can't starve RPBP for too
     * long. Remaining events just stay queued for the next tick. */
    const unsigned MAX_GPIO_PER_TICK = 8u;
    for (unsigned i = 0; i < MAX_GPIO_PER_TICK; ++i) {
        struct io_gpio_event ev;
        if (!ipc_queue_pop(&g_gpio_q, &ev)) break;

        if (emit_gpio_event(&ev)) g_stats.gpio_events_emitted++;
        else                       g_stats.gpio_events_dropped++;
    }

    /* Drain ADC samples into STREAM_DATA frames on the active stream
     * channel. Bounded per tick to keep USB airtime fair vs other
     * pumps. Samples arriving while no channel is configured are
     * silently discarded — the sampler may have been running before
     * the stream opened. */
    const unsigned MAX_ADC_PER_TICK = 16u;
    uint16_t chan = g_adc_stream_channel;
    for (unsigned i = 0; i < MAX_ADC_PER_TICK; ++i) {
        struct io_adc_sample s;
        if (!ipc_queue_pop(&g_adc_q, &s)) break;
        if (chan != 0xFFFFu && rpbp_stream_is_open(chan)) {
            emit_adc_stream(chan, &s);
        }
    }
}

/* ───── Core 1 entry point ───────────────────────────────────────────── */

/*
 * Core-1 scheduler loop. Services registered here run in this
 * thread; for M6b the only active service is the ADC streamer.
 *
 * Sampling strategy: re-read `g_adc_cfg` every tick so the host can
 * re-target channels / rate live without a stop-start dance. When
 * `period_us == 0` the sampler idles at 1 ms until the next config
 * arrives.
 *
 * adc_hw_read_raw() is re-entrant against Core-0 single-shot reads
 * in practice — the RP2350 ADC mux + SAR path is single-user and
 * Core-0 callers are rare (cmd_adc uses it). A full arbiter lives
 * with M6c when more Core-1 services start competing for the ADC.
 */
static void core1_entry(void)
{
    absolute_time_t next_tick = get_absolute_time();

    for (;;) {
        g_stats.core1_loops++;

        uint32_t cfg      = g_adc_cfg;
        uint8_t  mask     = (uint8_t)(cfg & 0xFFu);
        uint32_t period_us = cfg >> 8;

        if (mask == 0u || period_us == 0u) {
            sleep_ms(1);
            next_tick = get_absolute_time();
            continue;
        }

        struct io_adc_sample s = {
            .timestamp_us = (uint32_t)time_us_64(),
            .channel_mask = mask,
            .sample_count = (uint8_t)popcount8(mask),
        };
        unsigned out_i = 0u;
        for (uint8_t ch = 0u; ch < 5u && out_i < 4u; ++ch) {
            if ((mask & (1u << ch)) == 0u) continue;
            int raw = adc_hw_read_raw(ch);
            s.samples[out_i++] = (raw < 0) ? 0u : (uint16_t)raw;
        }

        if (ipc_queue_push(&g_adc_q, &s)) g_stats.adc_samples_produced++;
        else                              g_stats.adc_samples_dropped++;

        /* Drift-free cadence: schedule the next tick from the previous
         * deadline rather than "now + period", so sampling jitter does
         * not accumulate across ticks. */
        next_tick = delayed_by_us(next_tick, (uint64_t)period_us);
        int64_t remain = absolute_time_diff_us(get_absolute_time(), next_tick);
        if (remain > 0) sleep_us((uint64_t)remain);
        else            next_tick = get_absolute_time();   /* fell behind — resync */
    }
}

int io_control_start(void)
{
    ipc_queue_init(&g_gpio_q, g_gpio_q_storage,
                   sizeof(struct io_gpio_event),
                   IO_GPIO_Q_SLOTS);

    ipc_queue_init(&g_adc_q,  g_adc_q_storage,
                   sizeof(struct io_adc_sample),
                   IO_ADC_Q_SLOTS);

    memset(&g_stats, 0, sizeof(g_stats));
    g_adc_cfg = 0u;   /* sampler idle until adc_stream opcode arrives */

    /* Kick Core 1 into the scheduler. pico-sdk wires up the stack,
     * runs the SDK's core1 init, and hands control to our entry. */
    multicore_launch_core1(core1_entry);
    return 0;
}
