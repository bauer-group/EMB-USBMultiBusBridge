// SPDX-License-Identifier: GPL-2.0
/*
 * Capability-map decoder.
 *
 * The device answers CMD_REQUEST{SYS,GET_CAPABILITIES} with a CBOR
 * map (RFC 8949) documented in docs/protocol/capabilities.md. Keys
 * we recognise:
 *
 *   proto    : [u8, u8, u8]
 *   fw       : text
 *   board    : text
 *   serial   : bytes
 *   mtu      : { "out": u, "in": u }
 *   buses    : { "<class>": [ { "idx": u, "max_freq_hz": u? }, … ], … }
 *
 * The walker tolerates unknown keys (forward compatibility) but
 * rejects malformed CBOR structurally. Everything is bounds-checked
 * — every read advances the cursor only after verifying the bytes
 * are in range, and every container honours its declared length so
 * a hostile payload cannot make us walk off the end of @cbor.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "cap.h"

/* ───── CBOR walker (RFC 8949 §3) ────────────────────────────────── */

enum cbor_major {
	CBOR_UINT   = 0,
	CBOR_NEGINT = 1,
	CBOR_BYTES  = 2,
	CBOR_TEXT   = 3,
	CBOR_ARRAY  = 4,
	CBOR_MAP    = 5,
	CBOR_TAG    = 6,
	CBOR_SIMPLE = 7,
};

struct cbor {
	const u8 *buf;
	size_t    len;
	size_t    pos;
};

static int cbor_take(struct cbor *c, size_t n, const u8 **out)
{
	if (c->pos + n > c->len || c->pos + n < c->pos /* overflow */)
		return -EPROTO;
	*out = &c->buf[c->pos];
	c->pos += n;
	return 0;
}

/*
 * Read one CBOR head: returns the major type and the additional
 * argument value (length or integer payload). Caller decides how to
 * interpret the result based on @major.
 */
static int cbor_head(struct cbor *c, enum cbor_major *major, u64 *arg)
{
	const u8 *b;
	int ret;
	u8 head, info;

	ret = cbor_take(c, 1, &b);
	if (ret)
		return ret;
	head = *b;
	*major = (enum cbor_major)(head >> 5);
	info = head & 0x1F;

	if (info < 24) {
		*arg = info;
		return 0;
	}
	switch (info) {
	case 24:
		ret = cbor_take(c, 1, &b);
		if (ret) return ret;
		*arg = (u64)b[0];
		return 0;
	case 25:
		ret = cbor_take(c, 2, &b);
		if (ret) return ret;
		*arg = ((u64)b[0] << 8) | (u64)b[1];
		return 0;
	case 26:
		ret = cbor_take(c, 4, &b);
		if (ret) return ret;
		*arg = ((u64)b[0] << 24) | ((u64)b[1] << 16)
		     | ((u64)b[2] << 8)  | (u64)b[3];
		return 0;
	case 27:
		ret = cbor_take(c, 8, &b);
		if (ret) return ret;
		*arg = ((u64)b[0] << 56) | ((u64)b[1] << 48)
		     | ((u64)b[2] << 40) | ((u64)b[3] << 32)
		     | ((u64)b[4] << 24) | ((u64)b[5] << 16)
		     | ((u64)b[6] << 8)  | (u64)b[7];
		return 0;
	default:
		/* indefinite-length not used by the spec */
		return -EPROTO;
	}
}

/* Skip exactly one CBOR item including its descendants. */
static int cbor_skip(struct cbor *c)
{
	enum cbor_major major;
	u64 arg;
	int ret;
	u64 i;

	ret = cbor_head(c, &major, &arg);
	if (ret) return ret;

	switch (major) {
	case CBOR_UINT:
	case CBOR_NEGINT:
	case CBOR_SIMPLE:
		return 0;
	case CBOR_BYTES:
	case CBOR_TEXT:
		if (arg > c->len)
			return -EPROTO;
		if (c->pos + arg > c->len)
			return -EPROTO;
		c->pos += (size_t)arg;
		return 0;
	case CBOR_ARRAY:
		for (i = 0; i < arg; i++) {
			ret = cbor_skip(c);
			if (ret) return ret;
		}
		return 0;
	case CBOR_MAP:
		for (i = 0; i < arg; i++) {
			ret = cbor_skip(c); /* key */
			if (ret) return ret;
			ret = cbor_skip(c); /* value */
			if (ret) return ret;
		}
		return 0;
	case CBOR_TAG:
		return cbor_skip(c);
	}
	return -EPROTO;
}

/*
 * Read an unsigned integer. Returns -EPROTO on type mismatch.
 */
static int cbor_uint(struct cbor *c, u64 *out)
{
	enum cbor_major major;
	u64 arg;
	int ret;

	ret = cbor_head(c, &major, &arg);
	if (ret) return ret;
	if (major != CBOR_UINT)
		return -EPROTO;
	*out = arg;
	return 0;
}

/*
 * Read a text string. Returns a pointer into @c->buf — caller must
 * not retain it past @c's lifetime.
 */
static int cbor_text(struct cbor *c, const u8 **out, size_t *out_len)
{
	enum cbor_major major;
	u64 arg;
	const u8 *p;
	int ret;

	ret = cbor_head(c, &major, &arg);
	if (ret) return ret;
	if (major != CBOR_TEXT)
		return -EPROTO;
	if (arg > c->len)
		return -EPROTO;
	ret = cbor_take(c, (size_t)arg, &p);
	if (ret) return ret;
	*out = p;
	*out_len = (size_t)arg;
	return 0;
}

/* Compare a text item against a NUL-terminated literal. */
static bool cbor_text_eq(const u8 *txt, size_t txt_len, const char *lit)
{
	size_t lit_len = strlen(lit);

	return txt_len == lit_len && memcmp(txt, lit, lit_len) == 0;
}

/* Copy a text item into a fixed-size NUL-terminated buffer. Truncates
 * silently — every destination has a documented ceiling and we
 * actively avoid -EOVERFLOW on a forward-compat firmware that adds
 * extra characters. */
static void cbor_text_copy(char *dst, size_t dst_size,
			   const u8 *src, size_t src_len)
{
	size_t n;

	if (dst_size == 0)
		return;
	n = src_len < dst_size - 1 ? src_len : dst_size - 1;
	if (n)
		memcpy(dst, src, n);
	dst[n] = '\0';
}

/* Parse the value of a "buses" entry — a CBOR array of objects each
 * carrying at least { idx: u, max_freq_hz: u? }. The class string is
 * already known from the parent map's key. */
static int parse_bus_array(struct cbor *c, const u8 *class_txt,
			   size_t class_len, struct rpbp_caps *out)
{
	enum cbor_major major;
	u64 count, i;
	int ret;

	ret = cbor_head(c, &major, &count);
	if (ret) return ret;
	if (major != CBOR_ARRAY)
		return -EPROTO;

	for (i = 0; i < count; i++) {
		enum cbor_major inner_major;
		u64 inner_count, j;
		struct rpbp_caps_bus *bus;

		ret = cbor_head(c, &inner_major, &inner_count);
		if (ret) return ret;
		if (inner_major != CBOR_MAP)
			return -EPROTO;

		if (out->bus_count >= RPBP_CAPS_MAX_BUSES) {
			/* Already at our compile-time ceiling. Skip the
			 * remaining map entries to keep the cursor
			 * advanced; subsequent buses of the same class
			 * are silently dropped. */
			for (j = 0; j < inner_count; j++) {
				ret = cbor_skip(c); if (ret) return ret;
				ret = cbor_skip(c); if (ret) return ret;
			}
			continue;
		}

		bus = &out->buses[out->bus_count];
		cbor_text_copy(bus->class, sizeof(bus->class),
			       class_txt, class_len);
		bus->idx = 0;
		bus->max_freq_hz = 0;

		for (j = 0; j < inner_count; j++) {
			const u8 *key;
			size_t key_len;
			u64 v;

			ret = cbor_text(c, &key, &key_len);
			if (ret) return ret;

			if (cbor_text_eq(key, key_len, "idx")) {
				ret = cbor_uint(c, &v);
				if (ret) return ret;
				if (v > 0xFFFFu)
					return -EPROTO;
				bus->idx = (u16)v;
			} else if (cbor_text_eq(key, key_len, "max_freq_hz")) {
				ret = cbor_uint(c, &v);
				if (ret) return ret;
				if (v > 0xFFFFFFFFu)
					return -EPROTO;
				bus->max_freq_hz = (u32)v;
			} else {
				ret = cbor_skip(c);
				if (ret) return ret;
			}
		}
		out->bus_count++;
	}
	return 0;
}

/* Parse the "mtu" submap. */
static int parse_mtu(struct cbor *c, struct rpbp_caps *out)
{
	enum cbor_major major;
	u64 count, i;
	int ret;

	ret = cbor_head(c, &major, &count);
	if (ret) return ret;
	if (major != CBOR_MAP)
		return -EPROTO;

	for (i = 0; i < count; i++) {
		const u8 *key;
		size_t key_len;
		u64 v;

		ret = cbor_text(c, &key, &key_len);
		if (ret) return ret;

		if (cbor_text_eq(key, key_len, "out")) {
			ret = cbor_uint(c, &v);
			if (ret) return ret;
			if (v > 0xFFFFFFFFu)
				return -EPROTO;
			out->mtu_out = (u32)v;
		} else if (cbor_text_eq(key, key_len, "in")) {
			ret = cbor_uint(c, &v);
			if (ret) return ret;
			if (v > 0xFFFFFFFFu)
				return -EPROTO;
			out->mtu_in = (u32)v;
		} else {
			ret = cbor_skip(c);
			if (ret) return ret;
		}
	}
	return 0;
}

/* Parse the top-level "buses" map. */
static int parse_buses(struct cbor *c, struct rpbp_caps *out)
{
	enum cbor_major major;
	u64 count, i;
	int ret;

	ret = cbor_head(c, &major, &count);
	if (ret) return ret;
	if (major != CBOR_MAP)
		return -EPROTO;

	for (i = 0; i < count; i++) {
		const u8 *cls;
		size_t cls_len;

		ret = cbor_text(c, &cls, &cls_len);
		if (ret) return ret;

		if (cls_len >= RPBP_CAPS_MAX_CLASS_LEN) {
			/* Unknown / overlong class name. Skip it. */
			ret = cbor_skip(c);
			if (ret) return ret;
			continue;
		}
		ret = parse_bus_array(c, cls, cls_len, out);
		if (ret) return ret;
	}
	return 0;
}

int rpbp_caps_parse(const u8 *cbor, size_t len, struct rpbp_caps *out)
{
	struct cbor c = { .buf = cbor, .len = len, .pos = 0 };
	enum cbor_major major;
	u64 count, i;
	int ret;

	if (!out)
		return -EINVAL;
	if (len > 0 && !cbor)
		return -EINVAL;
	if (len > 4096)
		return -EMSGSIZE;

	memset(out, 0, sizeof(*out));

	if (len == 0)
		return 0;

	ret = cbor_head(&c, &major, &count);
	if (ret) return ret;
	if (major != CBOR_MAP)
		return -EPROTO;

	for (i = 0; i < count; i++) {
		const u8 *key;
		size_t key_len;

		ret = cbor_text(&c, &key, &key_len);
		if (ret) return ret;

		if (cbor_text_eq(key, key_len, "proto")) {
			enum cbor_major arr_major;
			u64 arr_count, j;
			ret = cbor_head(&c, &arr_major, &arr_count);
			if (ret) return ret;
			if (arr_major != CBOR_ARRAY)
				return -EPROTO;
			for (j = 0; j < arr_count && j < 3; j++) {
				u64 v;
				ret = cbor_uint(&c, &v);
				if (ret) return ret;
				if (v > 0xFFu) return -EPROTO;
				out->proto[j] = (u8)v;
			}
			for (; j < arr_count; j++) {
				ret = cbor_skip(&c);
				if (ret) return ret;
			}
		} else if (cbor_text_eq(key, key_len, "fw")) {
			const u8 *txt; size_t txt_len;
			ret = cbor_text(&c, &txt, &txt_len);
			if (ret) return ret;
			cbor_text_copy(out->fw, sizeof(out->fw),
				       txt, txt_len);
		} else if (cbor_text_eq(key, key_len, "board")) {
			const u8 *txt; size_t txt_len;
			ret = cbor_text(&c, &txt, &txt_len);
			if (ret) return ret;
			cbor_text_copy(out->board, sizeof(out->board),
				       txt, txt_len);
		} else if (cbor_text_eq(key, key_len, "serial")) {
			enum cbor_major sm; u64 sa; const u8 *sp;
			ret = cbor_head(&c, &sm, &sa);
			if (ret) return ret;
			if (sm != CBOR_BYTES)
				return -EPROTO;
			if (sa > sizeof(out->serial))
				sa = sizeof(out->serial);
			ret = cbor_take(&c, (size_t)sa, &sp);
			if (ret) return ret;
			memcpy(out->serial, sp, (size_t)sa);
			out->serial_len = (size_t)sa;
		} else if (cbor_text_eq(key, key_len, "mtu")) {
			ret = parse_mtu(&c, out);
			if (ret) return ret;
		} else if (cbor_text_eq(key, key_len, "buses")) {
			ret = parse_buses(&c, out);
			if (ret) return ret;
		} else {
			ret = cbor_skip(&c);
			if (ret) return ret;
		}
	}
	return 0;
}
