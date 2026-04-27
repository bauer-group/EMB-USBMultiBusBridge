/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * io_control — the second-core I/O scheduler.
 *
 * Responsibility split (see docs/firmware/overview.md):
 *   Core 0 — TinyUSB event loop, RPBP framing, command dispatch,
 *            capability map, pin broker, LED scheduler, non-time-
 *            critical housekeeping.
 *   Core 1 — I/O scheduler. Periodic ADC sampling, logic capture,
 *            deferred event producers, TIME_SYNC emission. Owns its
 *            own DMA channels and PIO IRQ handlers.
 *
 * Inter-core communication uses lock-free SPSC rings (ipc_queue.h)
 * so Core 0 never needs to spin-wait on Core 1. Each producer/
 * consumer pair identifies exactly one thread on each side.
 *
 * This header exposes the bring-up entry points and the queue
 * endpoints that cross the core boundary. Services register
 * themselves inside io_control.c (compile-time list for the v1
 * scaffold).
 */

#ifndef RPBRIDGE_IO_CONTROL_H
#define RPBRIDGE_IO_CONTROL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ipc_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GPIO edge event — produced by the IRQ handler on whichever core
 * installed it (Core 0 today), consumed by the main-loop pump that
 * emits RPBP EVENT frames on channel 1.
 */
struct io_gpio_event {
    uint32_t timestamp_us;       /* time_us_64() & 0xFFFFFFFF       */
    uint8_t  pin;                /* 0..47                           */
    uint8_t  level;              /* 0 = falling edge, 1 = rising    */
    uint8_t  reserved[2];
};

/* GPIO event queue — producer: gpio IRQ, consumer: main loop. */
struct ipc_queue *io_control_gpio_queue(void);

/*
 * ADC stream sample produced by the Core-1 sampler. One struct
 * represents one acquisition tick; `samples[i]` pairs with the
 * i'th set bit in `channel_mask`.
 *
 * Size kept at 16 bytes so the SPSC ring entry-size matches a cache
 * line fraction and pow-of-2 slot counts give predictable footprint.
 */
struct io_adc_sample {
    uint32_t timestamp_us;   /* time_us_64() & 0xFFFFFFFF         */
    uint8_t  channel_mask;   /* bits 0..4, mirrors ADC_READ_MANY  */
    uint8_t  sample_count;   /* popcount(channel_mask), cached    */
    uint8_t  reserved[2];
    uint16_t samples[4];     /* up to 4 external channels per tick */
};

/* ADC sample ring — producer: Core 1, consumer: Core 0 main loop. */
struct ipc_queue *io_control_adc_queue(void);

/*
 * Configure the Core-1 ADC sampler. `channel_mask` selects which
 * ADC channels to read each tick (bit 0 = ch 0 / GP26, ..., bit 4 =
 * on-chip temperature). `period_us` is the sample period; 0 stops
 * the sampler. Takes effect on the next Core-1 tick.
 *
 * Call from Core 0 only. Safe to call while the sampler is running
 * (atomic u32 update on a shared config word).
 */
void io_control_adc_configure(uint8_t channel_mask, uint32_t period_us);

/* Fetch the current ADC sampler configuration. */
void io_control_adc_config(uint8_t *channel_mask_out, uint32_t *period_us_out);

/*
 * Set the RPBP dynamic channel that the main-loop ADC drain emits
 * STREAM_DATA frames on. `channel == 0xFFFF` disables emission (the
 * drain pops samples and discards them — cheap). Write once when
 * opening the stream; the pump honours the new target on the next
 * io_control_poll() tick.
 */
void io_control_adc_stream_channel(uint16_t channel);

uint16_t io_control_adc_stream_channel_get(void);

/*
 * Start Core 1 and bring the I/O scheduler online. Must be called
 * exactly once from Core 0 after the pin broker + USB stack are up
 * (Core 1 runs its own idle tick and waits for service registrations
 * to arrive via shared-memory hand-shake).
 *
 * Returns 0 on success, -RPBP_EIO if Core 1 refused to start.
 */
int io_control_start(void);

/*
 * Main-loop pump: drain cross-core queues, emit RPBP events/frames.
 * Must be called regularly from Core 0. Non-blocking; returns
 * quickly if no work is queued.
 */
void io_control_poll(void);

/* Optional debug / diagnostics. */
struct io_control_stats {
    uint32_t gpio_events_emitted;
    uint32_t gpio_events_dropped;
    uint32_t core1_loops;
    uint32_t adc_samples_produced;
    uint32_t adc_samples_dropped;   /* producer-side ring overflow    */
};

void io_control_stats(struct io_control_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_IO_CONTROL_H */
