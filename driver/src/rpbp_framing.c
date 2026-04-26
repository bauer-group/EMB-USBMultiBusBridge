// SPDX-License-Identifier: GPL-2.0
/*
 * RPBP v1 framing — see rpbp_framing.h.
 *
 * Every public function performs explicit size + null checks before
 * touching the input buffer. The driver runs in kernel mode where a
 * single off-by-one OOB read can leak unmapped memory; the byte-level
 * defensiveness here is intentional and not redundant.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "rpbp.h"
#include "rpbp_crc32c.h"
#include "rpbp_framing.h"

/* ───── Helpers ──────────────────────────────────────────────────── */

static inline u16 rd_le16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static inline u32 rd_le32(const u8 *p)
{
	return (u32)p[0]        | ((u32)p[1] << 8)
	     | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline void wr_le16(u8 *p, u16 v)
{
	p[0] = (u8)(v & 0xFFu);
	p[1] = (u8)((v >> 8) & 0xFFu);
}

static inline void wr_le32(u8 *p, u32 v)
{
	p[0] = (u8)(v & 0xFFu);
	p[1] = (u8)((v >> 8) & 0xFFu);
	p[2] = (u8)((v >> 16) & 0xFFu);
	p[3] = (u8)((v >> 24) & 0xFFu);
}

/* ───── Public API ───────────────────────────────────────────────── */

int rpbp_header_encode(const struct rpbp_header *h, u8 *dst, size_t dst_len)
{
	if (!h || !dst)
		return -EINVAL;
	if (dst_len < RPBP_HEADER_LEN)
		return -EINVAL;

	dst[0] = h->magic;
	dst[1] = h->version;
	dst[2] = h->msg_type;
	dst[3] = h->flags;
	wr_le16(&dst[4],  h->channel);
	wr_le16(&dst[6],  h->seq);
	wr_le32(&dst[8],  h->payload_len);
	wr_le32(&dst[12], h->timestamp_us);
	return 0;
}

int rpbp_header_parse(const u8 *src, size_t src_len, struct rpbp_header *out)
{
	if (!src || !out)
		return -EINVAL;
	if (src_len < RPBP_HEADER_LEN)
		return -EPROTO;

	if (src[0] != RPBP_MAGIC || src[1] != RPBP_VERSION)
		return -EPROTO;
	if (src[3] & RPBP_FLAG_RESERVED_MASK)
		return -EPROTO;

	out->magic        = src[0];
	out->version      = src[1];
	out->msg_type     = src[2];
	out->flags        = src[3];
	out->channel      = rd_le16(&src[4]);
	out->seq          = rd_le16(&src[6]);
	out->payload_len  = rd_le32(&src[8]);
	out->timestamp_us = rd_le32(&src[12]);

	/* FRAGMENT + LAST is a contradiction in v1. */
	if ((out->flags & RPBP_FLAG_FRAGMENT) &&
	    (out->flags & RPBP_FLAG_LAST))
		return -EPROTO;

	if (out->payload_len > RPBP_MAX_PAYLOAD_LEN)
		return -EMSGSIZE;

	return 0;
}

int rpbp_frame_build(struct rpbp_header *hdr,
		     const u8 *payload, size_t payload_len,
		     u8 *dst, size_t dst_len, size_t *out_len)
{
	size_t total;
	u32 crc;
	int ret;

	if (!hdr || !dst || !out_len)
		return -EINVAL;
	if (payload_len > 0 && !payload)
		return -EINVAL;
	if (payload_len > RPBP_MAX_PAYLOAD_LEN)
		return -EMSGSIZE;

	total = RPBP_HEADER_LEN + payload_len + RPBP_CRC_LEN;
	if (dst_len < total)
		return -EINVAL;

	/* Spec-mandated header fields filled by us, others preserved. */
	hdr->magic       = RPBP_MAGIC;
	hdr->version     = RPBP_VERSION;
	hdr->payload_len = (u32)payload_len;

	ret = rpbp_header_encode(hdr, dst, dst_len);
	if (ret)
		return ret;

	if (payload_len > 0)
		memcpy(&dst[RPBP_HEADER_LEN], payload, payload_len);

	crc = rpbp_crc32c(dst, RPBP_HEADER_LEN + payload_len);
	wr_le32(&dst[RPBP_HEADER_LEN + payload_len], crc);

	*out_len = total;
	return 0;
}

int rpbp_frame_validate(const u8 *frame, size_t frame_len,
			struct rpbp_header *out)
{
	struct rpbp_header h;
	size_t want_len;
	u32 got_crc, want_crc;
	int ret;

	if (!frame || !out)
		return -EINVAL;
	if (frame_len < (size_t)RPBP_HEADER_LEN + RPBP_CRC_LEN)
		return -EPROTO;

	ret = rpbp_header_parse(frame, frame_len, &h);
	if (ret)
		return ret;

	want_len = (size_t)RPBP_HEADER_LEN + h.payload_len + RPBP_CRC_LEN;
	if (frame_len != want_len)
		return -EMSGSIZE;

	got_crc = rd_le32(&frame[RPBP_HEADER_LEN + h.payload_len]);
	want_crc = rpbp_crc32c(frame, RPBP_HEADER_LEN + h.payload_len);
	if (got_crc != want_crc)
		return -EBADMSG;

	*out = h;
	return 0;
}

const u8 *rpbp_frame_payload(const u8 *frame, const struct rpbp_header *h)
{
	if (!frame || !h)
		return NULL;
	return &frame[RPBP_HEADER_LEN];
}

int rpbp_status_to_errno(enum rpbp_status s)
{
	switch (s) {
	case RPBP_OK:         return 0;
	case RPBP_EAGAIN:     return -EAGAIN;
	case RPBP_EINVAL:     return -EINVAL;
	case RPBP_EPERM:      return -EPERM;
	case RPBP_ENOENT:     return -ENOENT;
	case RPBP_EIO:        return -EIO;
	case RPBP_ETIMEDOUT:  return -ETIMEDOUT;
	case RPBP_EMSGSIZE:   return -EMSGSIZE;
	case RPBP_ECRC:       return -EBADMSG;
	case RPBP_EPROTO:     return -EPROTO;
	case RPBP_ENOTSUP:    return -EOPNOTSUPP;
	case RPBP_EBUSY:      return -EBUSY;
	case RPBP_ECANCELED:  return -ECANCELED;
	case RPBP_ENOSPC:     return -ENOSPC;
	case RPBP_EOVERFLOW:  return -EOVERFLOW;
	case RPBP_ENODEV:     return -ENODEV;
	}
	/* Future MINOR may add codes >= 16. Per spec, fallback to -EIO
	 * (operation failed, cause not in our table) instead of -EPROTO
	 * (which would imply protocol corruption). */
	return -EIO;
}
EXPORT_SYMBOL_GPL(rpbp_status_to_errno);
