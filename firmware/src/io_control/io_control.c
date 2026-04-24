/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * io_control — second-core scheduler + cross-core event pump.
 */

#include "io_control.h"

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#include "pico/multicore.h"
#include "pico/time.h"

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

void io_control_poll(void)
{
    /* Drain up to a bounded number of GPIO events per poll so a
     * misbehaving switch-bounce storm can't starve RPBP for too
     * long. Remaining events just stay queued for the next tick. */
    const unsigned MAX_PER_TICK = 8u;

    for (unsigned i = 0; i < MAX_PER_TICK; ++i) {
        struct io_gpio_event ev;
        if (!ipc_queue_pop(&g_gpio_q, &ev)) break;

        if (emit_gpio_event(&ev)) g_stats.gpio_events_emitted++;
        else                       g_stats.gpio_events_dropped++;
    }
}

/* ───── Core 1 entry point ───────────────────────────────────────────── */

/*
 * Idle scheduler. For the first M6 cut this is a tight sleep loop —
 * it exists to prove the core comes online and to anchor future
 * services (ADC streamer, logic analyser, TIME_SYNC emitter) that
 * register themselves by patching this list.
 *
 * Runs at the Core 1 default clock (same as clk_sys on RP2350). Uses
 * sleep_us() so the core's power draw is modest when idle — a quick
 * wake-up every 1 ms keeps service responsiveness tight without
 * burning the battery.
 */
static void core1_entry(void)
{
    for (;;) {
        g_stats.core1_loops++;
        sleep_us(1000);
    }
}

int io_control_start(void)
{
    ipc_queue_init(&g_gpio_q, g_gpio_q_storage,
                   sizeof(struct io_gpio_event),
                   IO_GPIO_Q_SLOTS);

    memset(&g_stats, 0, sizeof(g_stats));

    /* Kick Core 1 into the scheduler. pico-sdk wires up the stack,
     * runs the SDK's core1 init, and hands control to our entry. */
    multicore_launch_core1(core1_entry);
    return 0;
}
