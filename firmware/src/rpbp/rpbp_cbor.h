/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * rpbp_cbor — minimal CBOR writer (RFC 8949) used by the firmware to
 * emit the capability map, the identity map, and CBOR-flagged
 * command responses.
 *
 * Scope:
 *   - writer only (the firmware never parses CBOR from the host)
 *   - definite-length encoding only (known-size maps and arrays)
 *   - supported major types: 0 (uint), 2 (byte string), 3 (text), 4
 *     (array), 5 (map), 7 (simple: false/true/null)
 *   - supported primitive payloads up to 64 bits (u64)
 *
 * Errors are sticky — once the writer overflows its buffer or is fed
 * an oversized value it records the failure and every subsequent
 * call becomes a no-op. Callers check via cbor_writer_ok() once, at
 * the end.
 */

#ifndef RPBP_CBOR_H
#define RPBP_CBOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cbor_writer {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     ok;
};

/*
 * Initialise a writer over `buf` with capacity `cap` bytes. Starting
 * state is `ok = true, pos = 0`.
 */
void cbor_writer_init(struct cbor_writer *w, uint8_t *buf, size_t cap);

/* Has every write so far succeeded? */
static inline bool cbor_writer_ok(const struct cbor_writer *w)
{
    return w->ok;
}

/* Bytes written so far (0..cap). Undefined if !ok. */
static inline size_t cbor_writer_used(const struct cbor_writer *w)
{
    return w->pos;
}

/* Unsigned integer (major type 0). Accepts full u64 range. */
void cbor_writer_uint(struct cbor_writer *w, uint64_t v);

/* Negative integer via (major type 1). `v` is the absolute value of
 * the negative number minus 1: encoding -5 means v=4 here. */
void cbor_writer_nint(struct cbor_writer *w, uint64_t v);

/* UTF-8 text string (major type 3). */
void cbor_writer_text(struct cbor_writer *w, const char *s, size_t len);
void cbor_writer_text_cstr(struct cbor_writer *w, const char *cstr);

/* Byte string (major type 2). */
void cbor_writer_bytes(struct cbor_writer *w, const uint8_t *data, size_t len);

/* Array header (major type 4). Caller then emits `count` items. */
void cbor_writer_array(struct cbor_writer *w, size_t count);

/* Map header (major type 5). Caller emits `count` key/value pairs. */
void cbor_writer_map(struct cbor_writer *w, size_t count);

/* Booleans and null (major type 7 simple values). */
void cbor_writer_bool(struct cbor_writer *w, bool b);
void cbor_writer_null(struct cbor_writer *w);

/*
 * Convenience: emit a `"key": uint` map entry.
 * Equivalent to cbor_writer_text_cstr(w,key) + cbor_writer_uint(w,v).
 */
static inline void cbor_writer_kv_uint(struct cbor_writer *w,
                                       const char *key, uint64_t v)
{
    cbor_writer_text_cstr(w, key);
    cbor_writer_uint(w, v);
}

static inline void cbor_writer_kv_text(struct cbor_writer *w,
                                       const char *key, const char *cstr)
{
    cbor_writer_text_cstr(w, key);
    cbor_writer_text_cstr(w, cstr);
}

static inline void cbor_writer_kv_bytes(struct cbor_writer *w,
                                        const char *key,
                                        const uint8_t *data, size_t len)
{
    cbor_writer_text_cstr(w, key);
    cbor_writer_bytes(w, data, len);
}

static inline void cbor_writer_kv_bool(struct cbor_writer *w,
                                       const char *key, bool b)
{
    cbor_writer_text_cstr(w, key);
    cbor_writer_bool(w, b);
}

#ifdef __cplusplus
}
#endif
#endif /* RPBP_CBOR_H */
