/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * RPBP v1 — header encode/decode + frame validation and assembly.
 *
 * Endian policy: all multi-byte fields are little-endian on the wire.
 * Big-endian hosts (s390x and similar) work without changes because we
 * serialise field by field rather than casting struct pointers.
 */

#include "rpbp_framing.h"
#include "rpbp_crc32c.h"

#include <string.h>

/* ───── small LE helpers ─────────────────────────────────────────────── */

static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0]         |
           ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ───── Encode ───────────────────────────────────────────────────────── */

void rpbp_header_encode(const struct rpbp_header *h,
                        uint8_t dst[RPBP_HEADER_LEN])
{
    dst[0] = h->magic;
    dst[1] = h->version;
    dst[2] = h->msg_type;
    dst[3] = h->flags;
    wr_le16(&dst[4], h->channel);
    wr_le16(&dst[6], h->seq);
    wr_le32(&dst[8], h->payload_len);
    wr_le32(&dst[12], h->timestamp_us);
}

/* ───── Parse ────────────────────────────────────────────────────────── */

enum rpbp_status rpbp_header_parse(const uint8_t src[RPBP_HEADER_LEN],
                                   struct rpbp_header *out)
{
    if (src[0] != RPBP_MAGIC)   return RPBP_EPROTO;
    if (src[1] != RPBP_VERSION) return RPBP_EPROTO;

    const uint8_t flags = src[3];
    if (!rpbp_flags_valid(flags)) return RPBP_EPROTO;

    out->magic        = src[0];
    out->version      = src[1];
    out->msg_type     = src[2];
    out->flags        = flags;
    out->channel      = rd_le16(&src[4]);
    out->seq          = rd_le16(&src[6]);
    out->payload_len  = rd_le32(&src[8]);
    out->timestamp_us = rd_le32(&src[12]);

    if (out->payload_len > RPBP_MAX_PAYLOAD_LEN) {
        return RPBP_EMSGSIZE;
    }
    return RPBP_OK;
}

/* ───── Seal (trailing CRC) ──────────────────────────────────────────── */

void rpbp_frame_seal(const uint8_t *hdr_plus_payload, size_t len,
                     uint8_t crc_out[RPBP_CRC_LEN])
{
    uint32_t c = rpbp_crc32c(hdr_plus_payload, len);
    wr_le32(crc_out, c);
}

/* ───── Validate fully received frame ────────────────────────────────── */

enum rpbp_status rpbp_frame_validate(const uint8_t *frame, size_t frame_len,
                                     struct rpbp_header *hdr_out)
{
    if (frame_len < RPBP_HEADER_LEN + RPBP_CRC_LEN) {
        return RPBP_EMSGSIZE;
    }

    enum rpbp_status s = rpbp_header_parse(frame, hdr_out);
    if (s != RPBP_OK) {
        return s;
    }

    const size_t expected = (size_t)RPBP_HEADER_LEN +
                            hdr_out->payload_len    +
                            RPBP_CRC_LEN;
    if (frame_len != expected) {
        return RPBP_EMSGSIZE;
    }

    const uint32_t want = rd_le32(&frame[RPBP_HEADER_LEN + hdr_out->payload_len]);
    const uint32_t got  = rpbp_crc32c(frame,
                                      RPBP_HEADER_LEN + hdr_out->payload_len);
    if (want != got) {
        return RPBP_ECRC;
    }

    return RPBP_OK;
}

/* ───── TX build helper ──────────────────────────────────────────────── */

size_t rpbp_frame_build(uint8_t *dst,
                        struct rpbp_header *hdr,
                        const uint8_t *payload,
                        size_t payload_len)
{
    if (payload_len > RPBP_MAX_PAYLOAD_LEN)
        return 0;
    if (!rpbp_flags_valid(hdr->flags))
        return 0;

    hdr->magic       = RPBP_MAGIC;
    hdr->version     = RPBP_VERSION;
    hdr->payload_len = (uint32_t)payload_len;

    rpbp_header_encode(hdr, dst);

    if (payload_len > 0 && payload != NULL) {
        memcpy(dst + RPBP_HEADER_LEN, payload, payload_len);
    }

    uint8_t crc[RPBP_CRC_LEN];
    rpbp_frame_seal(dst, RPBP_HEADER_LEN + payload_len, crc);
    memcpy(dst + RPBP_HEADER_LEN + payload_len, crc, RPBP_CRC_LEN);

    return RPBP_HEADER_LEN + payload_len + RPBP_CRC_LEN;
}
