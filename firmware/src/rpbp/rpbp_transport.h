/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * rpbp_transport — TinyUSB vendor-class glue for the RPBP control pipe.
 *
 * The control pipe is the first vendor interface (TinyUSB index 0 —
 * see firmware/src/usb/usb_descriptors.c). A second vendor interface
 * (index 1) carries the gs_usb CAN protocol and is handled elsewhere.
 *
 * This layer is strictly a byte shuttle: it drains incoming bytes into
 * a frame buffer, applies the RPBP framing rules (header-then-body),
 * validates each complete frame, and hands it to the dispatcher. On
 * the TX side it offers a single blocking-ish `rpbp_transport_send()`
 * used by the reply builders in rpbp_dispatch.c.
 *
 * There is no fragmentation state here — the dispatcher owns multi-
 * frame reassembly for messages that need it. The transport's job
 * stops at "one CRC-valid frame".
 */

#ifndef RPBP_TRANSPORT_H
#define RPBP_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise the transport. Registers itself with the dispatcher so
 * replies can be sent back. Must be called exactly once after
 * tusb_init() and before the main loop.
 */
void rpbp_transport_init(void);

/*
 * Non-blocking pump. Call from the main loop after tud_task(). Drains
 * any bytes waiting on the vendor OUT endpoint, advances the RX state
 * machine, and dispatches complete frames.
 *
 * Returns the number of complete frames dispatched in this call.
 */
unsigned rpbp_transport_poll(void);

/*
 * Blocking send of one RPBP frame. Returns 0 on success, -1 on error
 * (interface not mounted, buffer exhausted after retries).
 *
 * The RX and TX endpoints are independent, so this is safe to call
 * from inside a handler invoked by rpbp_transport_poll().
 */
int rpbp_transport_send(const uint8_t *frame, size_t frame_len);

/*
 * Total counters for diagnostics. Expose via SYS_SELFTEST or a
 * future counters query.
 */
struct rpbp_transport_stats {
    uint32_t rx_frames_ok;
    uint32_t rx_frames_bad;   /* CRC or framing error */
    uint32_t rx_resyncs;      /* bytes dropped while scanning for MAGIC */
    uint32_t tx_frames;
    uint32_t tx_drops;
};

const struct rpbp_transport_stats *rpbp_transport_stats(void);

#ifdef __cplusplus
}
#endif
#endif /* RPBP_TRANSPORT_H */
