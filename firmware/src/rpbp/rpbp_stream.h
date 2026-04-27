/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * rpbp_stream — dynamic RPBP stream-channel registry + credit
 * tracking + STREAM_DATA emission helper.
 *
 * Responsibility split
 * ────────────────────
 *   This module owns two things:
 *     1. A small per-firmware registry (up to RPBP_STREAM_MAX slots)
 *        that maps a dynamic channel number (in [CHAN_DYN_MIN,
 *        CHAN_DYN_MAX]) to a per-stream state: credit counter, TX
 *        sequence, drop counters.
 *     2. The STREAM_DATA emission path. Producers (ADC streamer, logic
 *        capture, ...) call `rpbp_stream_send_frame()` with a payload
 *        of the appropriate subsystem-specific shape. This module
 *        checks credit, builds the RPBP header, calls the transport.
 *
 * Flow control
 * ────────────
 *   The host advertises credit by sending RPBP_MT_STREAM_CREDIT
 *   frames on the stream's channel with payload [delta LE u32]. The
 *   dispatcher forwards these via `rpbp_stream_on_credit()`, which
 *   adds to the per-stream credit counter. Each STREAM_DATA frame
 *   decrements credit by its payload length; frames that would send
 *   negative credit are dropped + counted in stats.
 *
 * Concurrency model
 * ─────────────────
 *   The registry is not re-entrant. Call from Core 0 main-loop
 *   context only. Core 1 producers enqueue samples into an IPC ring
 *   (see io_control) and Core 0's drain loop invokes
 *   `rpbp_stream_send_frame()`.
 */

#ifndef RPBRIDGE_RPBP_STREAM_H
#define RPBRIDGE_RPBP_STREAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPBP_STREAM_MAX            4u    /* simultaneous streams  */
#define RPBP_STREAM_INITIAL_CREDIT 8192u /* bytes per stream      */

/* Reset every slot to "free". Must run once during firmware init
 * before any opcode can open a stream. Idempotent. */
void rpbp_stream_init(void);

/*
 * Open a new stream on the given dynamic channel. Returns:
 *   0           — opened, ready for send_frame().
 *   -RPBP_EINVAL — channel out of dynamic range.
 *   -RPBP_EBUSY  — channel already in use or registry full.
 * Initial credit is RPBP_STREAM_INITIAL_CREDIT; host tops up with
 * STREAM_CREDIT frames.
 */
int rpbp_stream_open(uint16_t channel);

/* Close a stream. Returns 0 on success, -RPBP_ENOENT if the channel
 * is not open. Pending credit is discarded. */
int rpbp_stream_close(uint16_t channel);

bool rpbp_stream_is_open(uint16_t channel);

/*
 * Build + send a STREAM_DATA frame. `payload` is subsystem-specific
 * content — the module does NOT inspect it beyond the length.
 * Returns 0 on success, -RPBP_EAGAIN on insufficient credit (frame
 * dropped + counted), -RPBP_ENOENT on closed channel, -RPBP_EIO on
 * TX failure.
 */
int rpbp_stream_send_frame(uint16_t channel,
                           const uint8_t *payload, size_t payload_len);

/*
 * Deposit additional credit on a stream channel. Called by the
 * dispatcher when a RPBP_MT_STREAM_CREDIT frame is received.
 * `delta_bytes` is saturated at 32-bit; silently ignored if the
 * channel is not open.
 */
void rpbp_stream_on_credit(uint16_t channel, uint32_t delta_bytes);

/* Per-stream stats — read-only snapshot. */
struct rpbp_stream_stats {
    uint16_t channel;
    bool     open;
    uint32_t credit;          /* current credit balance           */
    uint32_t tx_frames;       /* frames successfully emitted      */
    uint32_t tx_bytes;
    uint32_t drop_credit;     /* frames dropped on credit < len   */
    uint32_t drop_tx_fail;    /* frames that failed in transport  */
    uint16_t next_seq;
};

/*
 * Copy stats for every slot (open or not) into `out`. Returns the
 * number of slots written, always RPBP_STREAM_MAX if `cap` permits.
 */
size_t rpbp_stream_snapshot(struct rpbp_stream_stats *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_RPBP_STREAM_H */
