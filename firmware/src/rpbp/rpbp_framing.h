/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Wire-format helpers for RPBP headers and full frames.
 * Implementation: rpbp_framing.c.
 */

#ifndef RPBP_FRAMING_H
#define RPBP_FRAMING_H

#include "rpbp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialise a header into the first RPBP_HEADER_LEN bytes of `dst`.
 * dst must have room for at least RPBP_HEADER_LEN bytes.
 */
void rpbp_header_encode(const struct rpbp_header *h,
                        uint8_t dst[RPBP_HEADER_LEN]);

/*
 * Parse a 16-byte header at `src`. Returns RPBP_OK on success,
 * RPBP_EPROTO for magic/version/flags/msg_type violations, or
 * RPBP_EMSGSIZE if payload_len exceeds the protocol maximum.
 *
 * src must point to at least RPBP_HEADER_LEN bytes.
 */
enum rpbp_status rpbp_header_parse(const uint8_t src[RPBP_HEADER_LEN],
                                   struct rpbp_header *out);

/*
 * Compute the trailing CRC-32C over header+payload and write it
 * little-endian into crc_out[0..3]. Expects the fully-packed bytes.
 */
void rpbp_frame_seal(const uint8_t *hdr_plus_payload,
                     size_t hdr_plus_payload_len,
                     uint8_t crc_out[RPBP_CRC_LEN]);

/*
 * Validate a fully received frame: header, length, trailing CRC.
 * Returns RPBP_OK on success, otherwise one of RPBP_EPROTO / RPBP_EMSGSIZE
 * / RPBP_ECRC. hdr_out gets the parsed header fields on success.
 */
enum rpbp_status rpbp_frame_validate(const uint8_t *frame,
                                     size_t frame_len,
                                     struct rpbp_header *hdr_out);

/*
 * Convenience helper used by TX paths: pack header, append payload,
 * seal with CRC.
 *
 * Arguments:
 *   - dst: output buffer, must hold at least HEADER_LEN + payload_len + CRC_LEN
 *   - hdr: header fields (magic/version/msg_type/flags/channel/seq/timestamp_us);
 *          payload_len in hdr is OVERWRITTEN with the value supplied via payload_len arg
 *   - payload: 0..payload_len bytes (may be NULL if payload_len == 0)
 *   - payload_len: bytes to copy
 *
 * Returns the total frame length (header + payload + CRC), or 0 on
 * error (e.g. payload_len exceeds RPBP_MAX_PAYLOAD_LEN).
 */
size_t rpbp_frame_build(uint8_t *dst,
                        struct rpbp_header *hdr,
                        const uint8_t *payload,
                        size_t payload_len);

#ifdef __cplusplus
}
#endif
#endif /* RPBP_FRAMING_H */
