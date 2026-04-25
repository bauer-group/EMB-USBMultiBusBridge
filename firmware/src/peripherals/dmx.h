/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * dmx — DMX512-A transmitter driven by a single PIO state machine
 * on PIO2.
 *
 * Single universe (idx = 0). 512 slots + start code (slot 0). Frames
 * are emitted in two stages: BREAK + MAB by direct GPIO drive (CPU
 * loop with sleep_us), then slot bytes shifted via the PIO TX FIFO.
 *
 * Operational model
 * ─────────────────
 *   1. Host configures pixel data via dmx_set_slots(offset, bytes, len)
 *      — updates the in-RAM universe buffer; no transmission yet.
 *   2. Host calls dmx_start(): driver begins continuous frame
 *      transmission at ~44 Hz (USITT spec lower bound, gives most
 *      receivers stable refresh). Each frame uses the current buffer
 *      contents as the slot payload.
 *   3. Mid-frame updates land in the next frame.
 *   4. dmx_stop() halts the transmitter, leaves the line idle HIGH.
 *
 * The frame loop runs on the main loop's tick (`dmx_tick()`). The
 * BREAK + MAB sequence blocks for ≈ 96 µs once per frame; that is
 * comfortably under the watchdog feed cadence.
 */

#ifndef RPBRIDGE_DMX_H
#define RPBRIDGE_DMX_H

#include "pico.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USITT-A maximum: 512 channels + 1 start code byte. */
#define DMX_UNIVERSE_SIZE   513u

/* Default BREAK / MAB durations. Spec minima are 88 µs and 8 µs;
 * longer values are safer for older receivers. */
#define DMX_DEFAULT_BREAK_US  100u
#define DMX_DEFAULT_MAB_US     12u

struct dmx_info {
    uint8_t  count;          /* universe count (always 1 in v1)        */
    uint8_t  pin;            /* GPIO carrying TX after the transceiver */
    uint8_t  running;        /* 0 = stopped, 1 = transmitting          */
    uint16_t slot_count;     /* configured slots (1..512)              */
    uint16_t break_us;
    uint16_t mab_us;
};

/*
 * Bring the DMX transmitter online: claims PIO2 SM 1, loads the
 * dmx_tx program, claims RPBRIDGE_DMX_TX_PIN on the broker. Sets the
 * universe to all-zero and stops the SM. Idempotent.
 *
 * Returns 0 on success, -RPBP_EBUSY on pin/PIO conflict, -RPBP_ENOTSUP
 * if the build does not include DMX support.
 */
int dmx_init(void);

/*
 * Update slot bytes in the universe buffer. `offset` is the slot
 * index (0 = start code, 1..512 = channels). `bytes` are copied as-is.
 *
 * Returns 0 on success, -RPBP_EINVAL on out-of-range offset/len.
 */
int dmx_set_slots(uint16_t offset, const uint8_t *bytes, size_t len);

/*
 * Set the configured slot count (1..512). The wire frame always emits
 * the start code (slot 0) followed by `slot_count` channel bytes.
 * Increasing the count zero-fills the freshly-included slots.
 */
int dmx_set_slot_count(uint16_t slot_count);

/* Begin / stop continuous frame transmission. */
int dmx_start(void);
int dmx_stop (void);

/* Adjust BREAK / MAB durations. Both are clamped to spec minima
 * (≥ 88 µs / ≥ 8 µs). Pass 0 to keep the current value. */
int dmx_set_timing(uint16_t break_us, uint16_t mab_us);

/* Snapshot of the current configuration. */
void dmx_get_info(struct dmx_info *out);

/*
 * Tick: called from the main loop. Drives the next frame when due.
 * Calling cadence ≥ 1 kHz keeps the transmitter at the spec-default
 * 44 Hz refresh rate (about one frame every 22.7 ms).
 */
void dmx_tick(void);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_DMX_H */
