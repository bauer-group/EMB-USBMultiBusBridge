/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * RPBP — RPBridge Protocol, v1.
 *
 * Wire format (little-endian):
 *
 *     +------- 16 B header -------+----- 0..N payload -----+-- 4 B CRC --+
 *     | magic=0x52 | ver | mtype  |                        | CRC-32C     |
 *     | flags      | channel (2)  |                        | Castagnoli  |
 *     | seq (2)                   |                        | over        |
 *     | payload_len (4)           |                        | header      |
 *     | ts_us (4, device-boot)    |                        | + payload   |
 *     +---------------------------+------------------------+-------------+
 *
 * Authoritative specification: docs/drafts/rpbridge-driver-stack.md §4 and
 * docs/protocol/rpbp-v1.md (once written). In case of conflict, the driver-
 * stack draft wins.
 */

#ifndef RPBP_H
#define RPBP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPBP_MAGIC              0x52u   /* 'R' */
#define RPBP_VERSION            0x01u
#define RPBP_HEADER_LEN         16u
#define RPBP_CRC_LEN            4u
#define RPBP_MTU_DEFAULT        512u
#define RPBP_MAX_PAYLOAD_LEN    4096u
#define RPBP_MAX_FRAME_LEN      (RPBP_HEADER_LEN + RPBP_MAX_PAYLOAD_LEN + RPBP_CRC_LEN)

/* Message types — canonical integer values, stable across protocol minors. */
enum rpbp_msg_type {
    RPBP_MT_HELLO          = 0x00,
    RPBP_MT_CAPABILITIES   = 0x01,
    RPBP_MT_CMD_REQUEST    = 0x02,
    RPBP_MT_CMD_RESPONSE   = 0x03,
    RPBP_MT_STREAM_DATA    = 0x04,
    RPBP_MT_STREAM_CREDIT  = 0x05,
    RPBP_MT_EVENT          = 0x06,
    RPBP_MT_PING           = 0x07,
    RPBP_MT_PONG           = 0x08,
    RPBP_MT_ERROR          = 0x09,
    RPBP_MT_RESET_CHANNEL  = 0x0A,
    RPBP_MT_TIME_SYNC      = 0x0B,
};

/* Header flag bits. */
#define RPBP_FL_CBOR            (1u << 0)
#define RPBP_FL_COMPRESSED      (1u << 1)
#define RPBP_FL_URGENT          (1u << 2)
#define RPBP_FL_FRAGMENT        (1u << 3)
#define RPBP_FL_LAST            (1u << 4)
#define RPBP_FL_CONTINUATION    (1u << 5)

/* Reserved channel numbers — see driver-stack draft §4.5. */
#define RPBP_CHAN_CONTROL       0u
#define RPBP_CHAN_EVENTS        1u
#define RPBP_CHAN_BROADCAST     0xFFFFu

/* POSIX-style status codes used by CMD_RESPONSE and ERROR payloads. */
enum rpbp_status {
    RPBP_OK         = 0,
    RPBP_EAGAIN     = 1,
    RPBP_EINVAL     = 2,
    RPBP_EPERM      = 3,
    RPBP_ENOENT     = 4,
    RPBP_EIO        = 5,
    RPBP_ETIMEDOUT  = 6,
    RPBP_EMSGSIZE   = 7,
    RPBP_ECRC       = 8,
    RPBP_EPROTO     = 9,
    RPBP_ENOTSUP    = 10,
    RPBP_EBUSY      = 11,
    RPBP_ECANCELED  = 12,
    RPBP_ENOSPC     = 13,
    RPBP_EOVERFLOW  = 14,
};

/*
 * Parsed/validated header. The wire form is always the raw 16 bytes;
 * this struct is the firmware's in-memory view after rpbp_header_parse().
 * Note: timestamp_us is filled in by the TX path from the local timer —
 * not by the caller.
 */
struct rpbp_header {
    uint8_t  magic;        /* == RPBP_MAGIC */
    uint8_t  version;      /* == RPBP_VERSION for v1 frames */
    uint8_t  msg_type;     /* enum rpbp_msg_type */
    uint8_t  flags;        /* RPBP_FL_* */
    uint16_t channel;
    uint16_t seq;
    uint32_t payload_len;  /* excludes header + CRC */
    uint32_t timestamp_us;
};

/*
 * Serialize the header into a 16-byte buffer, little-endian. `dst` must
 * point to at least RPBP_HEADER_LEN bytes of writable memory.
 */
void rpbp_header_encode(const struct rpbp_header *h,
                        uint8_t dst[RPBP_HEADER_LEN]);

/*
 * Parse a 16-byte header from the wire. Returns RPBP_OK on success,
 * RPBP_EPROTO if magic/version are wrong, RPBP_EMSGSIZE if payload_len
 * exceeds RPBP_MAX_PAYLOAD_LEN.
 */
enum rpbp_status rpbp_header_parse(const uint8_t src[RPBP_HEADER_LEN],
                                   struct rpbp_header *out);

/*
 * Compute the trailing CRC-32C over header + payload and write it as
 * little-endian 32-bit into `crc_out`. `hdr` must be a valid encoded
 * header (16 B) immediately followed by `payload_len` payload bytes in
 * host memory.
 */
void rpbp_frame_seal(const uint8_t *hdr_plus_payload, size_t hdr_plus_payload_len,
                     uint8_t crc_out[RPBP_CRC_LEN]);

/*
 * Full-frame validation: check magic/version, length bounds, and CRC.
 * Returns RPBP_OK on a well-formed frame.
 */
enum rpbp_status rpbp_frame_validate(const uint8_t *frame, size_t frame_len,
                                     struct rpbp_header *hdr_out);

#ifdef __cplusplus
}
#endif
#endif /* RPBP_H */
