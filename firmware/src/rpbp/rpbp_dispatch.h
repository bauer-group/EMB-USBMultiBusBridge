/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * rpbp_dispatch — central RPBP frame router.
 *
 * The transport layer hands each CRC-validated frame to
 * rpbp_dispatch_frame(). The dispatcher routes based on msg_type:
 *
 *   HELLO          -> rpbp_handle_hello()
 *   PING           -> rpbp_handle_ping()
 *   TIME_SYNC      -> rpbp_handle_ping() (same payload contract)
 *   RESET_CHANNEL  -> rpbp_handle_reset_channel()
 *   CMD_REQUEST    -> rpbp_handle_cmd() which routes by subsys byte
 *   CAPABILITIES / CMD_RESPONSE / PONG / ERROR / STREAM_*  -> ignored
 *                                                           (device does
 *                                                            not receive)
 *   Everything else -> error reply
 *
 * The dispatcher also provides reply-builder helpers for handlers:
 * rpbp_reply_cmd(), rpbp_reply_error(), rpbp_reply_pong(), etc. These
 * assemble a complete frame in a scratch buffer and hand it to the
 * registered transport TX callback.
 */

#ifndef RPBP_DISPATCH_H
#define RPBP_DISPATCH_H

#include "rpbp.h"
#include "rpbp_framing.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Transport TX hook. Implemented by rpbp_transport.c. Returns 0 on
 * success, negative on failure (e.g. endpoint busy, frame too large).
 */
typedef int (*rpbp_tx_fn)(const uint8_t *frame, size_t frame_len);

/*
 * Per-session state tracked by the dispatcher: sequence counters for
 * device-initiated traffic on control and events channels, session
 * open/closed flag, last-seen host HELLO timestamp for drift
 * diagnostics.
 */
struct rpbp_session {
    rpbp_tx_fn tx;
    bool       open;           /* HELLO complete? */
    uint16_t   next_ctrl_seq;  /* device -> host on channel 0 */
    uint16_t   next_event_seq; /* device -> host on channel 1 */
};

/*
 * Initialise the dispatcher / session state. Must be called once
 * before the first rpbp_dispatch_frame().
 */
void rpbp_dispatch_init(rpbp_tx_fn tx);

/*
 * Handle a fully validated incoming frame. `payload` may be NULL if
 * `payload_len == 0`. Returns RPBP_OK if the frame was dispatched
 * (even to a handler that returned an error to the host — that is a
 * successful dispatch), or a non-zero status if the frame was
 * malformed at the dispatcher level.
 */
enum rpbp_status rpbp_dispatch_frame(const struct rpbp_header *hdr,
                                     const uint8_t *payload,
                                     size_t payload_len);

/*
 * Reset dispatcher session state — for example after a USB unplug.
 */
void rpbp_dispatch_reset_session(void);

/*
 * Expose the session object so handlers can consume and update it.
 */
struct rpbp_session *rpbp_session(void);

/* ───── Reply builders ───────────────────────────────────────────────── */

/*
 * Compose + send a CMD_RESPONSE mirroring the request's channel and
 * seq. subsys, opcode, status are placed in the first three bytes
 * of the response payload, followed by `result_len` bytes of opcode-
 * specific result data.
 *
 * If `cbor_flag` is true, the response header carries RPBP_FL_CBOR;
 * the caller is responsible for ensuring `result` is a valid CBOR
 * payload that includes the map/object — the subsys/opcode/status
 * bytes are NOT prepended in that case. See rpbp-v1.md §10.2.
 */
int rpbp_reply_cmd(const struct rpbp_header *req,
                   uint8_t subsys, uint8_t opcode, uint8_t status,
                   const uint8_t *result, size_t result_len,
                   bool cbor_flag);

/*
 * Emit an ERROR frame tied to a specific request. `reason` is an
 * optional short UTF-8 string (NULL for none; truncated to 255 bytes).
 */
int rpbp_reply_error(const struct rpbp_header *req,
                     uint8_t status, const char *reason);

/*
 * Respond to a PING (or TIME_SYNC) with a PONG on the same channel
 * and seq as the request. `dev_rx_us` is the device-local microsecond
 * timestamp captured at the moment the request arrived; it becomes
 * `t2` in the PTP-lite round trip.
 */
int rpbp_reply_pong(const struct rpbp_header *req, uint32_t dev_rx_us);

/*
 * Send an asynchronous EVENT frame on channel 1. The device owns the
 * seq assignment. Returns 0 on success or negative on TX failure.
 */
int rpbp_send_event(uint8_t flags,
                    const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif
#endif /* RPBP_DISPATCH_H */
