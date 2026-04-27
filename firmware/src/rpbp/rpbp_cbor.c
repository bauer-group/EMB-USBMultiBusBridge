/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * Minimal CBOR writer (RFC 8949, definite-length only).
 *
 * Layout of an encoded item:
 *
 *     +----------------------+-----------------+
 *     | initial byte (IB)    | extra len bytes |
 *     +----------------------+-----------------+
 *
 * IB = (major_type << 5) | length_code, where length_code is:
 *   0..23  -> the value itself (no extra bytes)
 *   24     -> one following byte holds the value (0..255)
 *   25     -> two following bytes hold the value (big-endian)
 *   26     -> four following bytes (big-endian)
 *   27     -> eight following bytes (big-endian)
 *
 * For byte/text strings and arrays/maps the "value" is the count;
 * the payload bytes follow for byte/text strings.
 *
 * The writer is intentionally simple — there is no pretty-print
 * support, no indefinite-length, no streaming. A single linear
 * buffer and a single failure flag are all we carry.
 */

#include "rpbp_cbor.h"

#include <string.h>

/* Sticky-error-aware byte append. Returns true if the byte was
 * written, false if the writer is now in an error state (either
 * previously or due to overflow on this call). */
static bool w_put(struct cbor_writer *w, uint8_t b)
{
    if (!w->ok) return false;
    if (w->pos >= w->cap) {
        w->ok = false;
        return false;
    }
    w->buf[w->pos++] = b;
    return true;
}

static bool w_put_bytes(struct cbor_writer *w,
                        const uint8_t *data, size_t len)
{
    if (!w->ok) return false;
    if (w->pos + len > w->cap) {
        w->ok = false;
        return false;
    }
    if (len > 0 && data != NULL) {
        memcpy(&w->buf[w->pos], data, len);
    }
    w->pos += len;
    return true;
}

/* Emit `(major_type << 5) | len_code` plus up to 8 big-endian extension
 * bytes holding `value`. */
static void emit_type_and_value(struct cbor_writer *w,
                                uint8_t major_type,
                                uint64_t value)
{
    uint8_t tag = (uint8_t)((major_type & 0x7) << 5);

    if (value <= 23u) {
        w_put(w, (uint8_t)(tag | (uint8_t)value));
    } else if (value <= 0xFFu) {
        w_put(w, (uint8_t)(tag | 24u));
        w_put(w, (uint8_t)value);
    } else if (value <= 0xFFFFu) {
        w_put(w, (uint8_t)(tag | 25u));
        w_put(w, (uint8_t)(value >> 8));
        w_put(w, (uint8_t)value);
    } else if (value <= 0xFFFFFFFFu) {
        w_put(w, (uint8_t)(tag | 26u));
        w_put(w, (uint8_t)(value >> 24));
        w_put(w, (uint8_t)(value >> 16));
        w_put(w, (uint8_t)(value >> 8));
        w_put(w, (uint8_t)value);
    } else {
        w_put(w, (uint8_t)(tag | 27u));
        w_put(w, (uint8_t)(value >> 56));
        w_put(w, (uint8_t)(value >> 48));
        w_put(w, (uint8_t)(value >> 40));
        w_put(w, (uint8_t)(value >> 32));
        w_put(w, (uint8_t)(value >> 24));
        w_put(w, (uint8_t)(value >> 16));
        w_put(w, (uint8_t)(value >> 8));
        w_put(w, (uint8_t)value);
    }
}

/* ───── Public API ───────────────────────────────────────────────────── */

void cbor_writer_init(struct cbor_writer *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->pos = 0;
    w->ok  = (buf != NULL);
}

void cbor_writer_uint(struct cbor_writer *w, uint64_t v)
{
    emit_type_and_value(w, 0, v);
}

void cbor_writer_nint(struct cbor_writer *w, uint64_t v)
{
    emit_type_and_value(w, 1, v);
}

void cbor_writer_text(struct cbor_writer *w, const char *s, size_t len)
{
    emit_type_and_value(w, 3, (uint64_t)len);
    w_put_bytes(w, (const uint8_t *)s, len);
}

void cbor_writer_text_cstr(struct cbor_writer *w, const char *cstr)
{
    cbor_writer_text(w, cstr, cstr ? strlen(cstr) : 0u);
}

void cbor_writer_bytes(struct cbor_writer *w,
                       const uint8_t *data, size_t len)
{
    emit_type_and_value(w, 2, (uint64_t)len);
    w_put_bytes(w, data, len);
}

void cbor_writer_array(struct cbor_writer *w, size_t count)
{
    emit_type_and_value(w, 4, (uint64_t)count);
}

void cbor_writer_map(struct cbor_writer *w, size_t count)
{
    emit_type_and_value(w, 5, (uint64_t)count);
}

void cbor_writer_bool(struct cbor_writer *w, bool b)
{
    w_put(w, b ? (uint8_t)0xF5u : (uint8_t)0xF4u);
}

void cbor_writer_null(struct cbor_writer *w)
{
    w_put(w, (uint8_t)0xF6u);
}
