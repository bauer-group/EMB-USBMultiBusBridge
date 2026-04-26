/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RPBP v1 framing — header encode/parse, frame build/validate.
 *
 * Mirrors firmware/src/rpbp/rpbp_framing.{c,h}; parity is enforced
 * by the shared golden-vector corpus under tests/golden/.
 */

#ifndef RPBRIDGE_RPBP_FRAMING_H
#define RPBRIDGE_RPBP_FRAMING_H

#include <linux/types.h>

#include "rpbp.h"

/**
 * struct rpbp_header - parsed RPBP header.
 *
 * @magic:        always %RPBP_MAGIC.
 * @version:      protocol version (always %RPBP_VERSION for v1).
 * @msg_type:     one of enum rpbp_msg_type.
 * @flags:        bitmask of RPBP_FLAG_*.
 * @channel:      RPBP channel number (control = %RPBP_CHAN_CONTROL).
 * @seq:          sequence number (16-bit rolling, per channel).
 * @payload_len:  payload length in bytes; <= %RPBP_MAX_PAYLOAD_LEN.
 * @timestamp_us: device-boot-relative microseconds.
 */
struct rpbp_header {
	u8  magic;
	u8  version;
	u8  msg_type;
	u8  flags;
	u16 channel;
	u16 seq;
	u32 payload_len;
	u32 timestamp_us;
};

/**
 * rpbp_header_encode() - serialise @h into @dst (>= %RPBP_HEADER_LEN).
 *
 * Returns 0 on success, -EINVAL on insufficient space.
 */
int rpbp_header_encode(const struct rpbp_header *h, u8 *dst, size_t dst_len);

/**
 * rpbp_header_parse() - parse @src into @out.
 *
 * Validates magic + version + reserved flag bits. Does NOT check the
 * payload length against the message type — that's a higher-layer
 * concern. Caller still has to validate the trailing CRC.
 *
 * Returns 0 on success, -EPROTO on malformed input.
 */
int rpbp_header_parse(const u8 *src, size_t src_len, struct rpbp_header *out);

/**
 * rpbp_frame_build() - build a complete frame (header + payload + CRC).
 * @hdr:         pointer to a header — magic / version / payload_len
 *               are filled in from the spec, all other fields preserved.
 * @payload:     payload bytes; may be %NULL iff @payload_len == 0.
 * @payload_len: number of payload bytes; <= %RPBP_MAX_PAYLOAD_LEN.
 * @dst:         output buffer; must hold at least
 *               %RPBP_HEADER_LEN + @payload_len + %RPBP_CRC_LEN bytes.
 * @dst_len:     size of @dst.
 * @out_len:     receives the number of bytes written on success.
 *
 * Returns 0 on success, -EINVAL on size or argument violations.
 */
int rpbp_frame_build(struct rpbp_header *hdr,
		     const u8 *payload, size_t payload_len,
		     u8 *dst, size_t dst_len, size_t *out_len);

/**
 * rpbp_frame_validate() - validate a fully-received frame.
 * @frame:    full frame bytes (header + payload + CRC).
 * @frame_len: number of bytes at @frame.
 * @out:      receives the parsed header on success.
 *
 * Verifies size, magic, version, reserved flag bits, declared
 * payload length matches the buffer, and the trailing CRC-32C.
 *
 * Returns 0 on success, -EPROTO / -EBADMSG / -EMSGSIZE on the
 * matching failure mode.
 */
int rpbp_frame_validate(const u8 *frame, size_t frame_len,
			struct rpbp_header *out);

/**
 * rpbp_frame_payload() - return a pointer to the payload region of a
 * frame previously validated by rpbp_frame_validate().
 *
 * @frame must outlive the returned pointer.
 */
const u8 *rpbp_frame_payload(const u8 *frame, const struct rpbp_header *h);

#endif /* RPBRIDGE_RPBP_FRAMING_H */
