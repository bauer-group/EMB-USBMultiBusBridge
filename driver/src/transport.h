/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Transport layer — synchronous request/response over the RPBP
 * bulk IN/OUT endpoint pair. Serialised by
 * rpbridge_dev::transport_lock.
 */

#ifndef RPBRIDGE_TRANSPORT_H
#define RPBRIDGE_TRANSPORT_H

#include <linux/types.h>

#include "rpbp.h"

struct rpbridge_dev;

/* Default round-trip timeout. Sized to the USB-FS bulk round-trip
 * ceiling (~1 ms nominal) plus the firmware's worst-case ADC tick
 * budget under contention, with margin. */
#define RPBRIDGE_RT_TIMEOUT_MS   200u

/**
 * rpbridge_round_trip() - send a CMD_REQUEST and read the matching
 *                          CMD_RESPONSE on the control channel.
 * @dev:           per-device state.
 * @subsys:        target subsystem (one of enum rpbp_subsys).
 * @opcode:        per-subsystem opcode.
 * @args:          subsystem argument bytes (may be %NULL iff
 *                 @args_len == 0).
 * @args_len:      number of bytes at @args.
 * @reply:         caller-supplied buffer to receive the reply
 *                 result bytes. The status byte is consumed
 *                 internally; the buffer here gets bytes [3..]
 *                 of the wire payload.
 * @reply_capacity: size of @reply.
 * @reply_len:     receives the number of bytes written to @reply.
 *
 * On success returns 0 and fills @reply / @reply_len. The
 * device-reported status byte is mapped into a Linux errno via
 * rpbp_status_to_errno() and returned negatively if non-zero.
 *
 * Other failure modes:
 *   -EINVAL on argument violations
 *   -EMSGSIZE if reply > @reply_capacity
 *   -ETIMEDOUT on USB timeout
 *   -EBADMSG  on CRC mismatch
 *   -EPROTO   on header mismatch / unexpected msg type / seq mismatch
 *   any -E*   surfaced from usb_bulk_msg()
 */
int rpbridge_round_trip(struct rpbridge_dev *dev,
			enum rpbp_subsys subsys, u8 opcode,
			const u8 *args, size_t args_len,
			u8 *reply, size_t reply_capacity, size_t *reply_len);

#endif /* RPBRIDGE_TRANSPORT_H */
