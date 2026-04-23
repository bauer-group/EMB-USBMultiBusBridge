/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * CBOR writer tests — byte-exact comparison against hand-verified
 * expected encodings from RFC 8949 Appendix A ("Examples of encoded
 * CBOR data items").
 */

#include "test_util.h"
#include "rpbp/rpbp_cbor.h"

static int cmp_buf(const uint8_t *got, size_t got_len,
                   const uint8_t *want, size_t want_len,
                   const char *label)
{
    if (got_len != want_len) {
        fprintf(stderr, "  FAIL %s: length %zu != %zu\n",
                label, got_len, want_len);
        return 1;
    }
    if (memcmp(got, want, got_len) != 0) {
        fprintf(stderr, "  FAIL %s: byte mismatch. got ", label);
        for (size_t i = 0; i < got_len; i++) fprintf(stderr, "%02x", got[i]);
        fprintf(stderr, ", want ");
        for (size_t i = 0; i < want_len; i++) fprintf(stderr, "%02x", want[i]);
        fprintf(stderr, "\n");
        return 1;
    }
    return 0;
}

TEST_CASE(uint_zero_is_single_byte)
{
    uint8_t buf[8];
    struct cbor_writer w;
    cbor_writer_init(&w, buf, sizeof(buf));
    cbor_writer_uint(&w, 0);
    ASSERT_TRUE(cbor_writer_ok(&w));
    const uint8_t want[] = { 0x00 };
    if (cmp_buf(buf, cbor_writer_used(&w), want, sizeof(want), "uint 0")) return 1;
    return 0;
}

TEST_CASE(uint_24_uses_one_byte_extension)
{
    uint8_t buf[8];
    struct cbor_writer w;
    cbor_writer_init(&w, buf, sizeof(buf));
    cbor_writer_uint(&w, 24);
    const uint8_t want[] = { 0x18, 0x18 };
    if (cmp_buf(buf, cbor_writer_used(&w), want, sizeof(want), "uint 24")) return 1;
    return 0;
}

TEST_CASE(uint_big_endian_extension)
{
    uint8_t buf[16];
    struct cbor_writer w;
    cbor_writer_init(&w, buf, sizeof(buf));
    cbor_writer_uint(&w, 0x01020304u);
    const uint8_t want[] = { 0x1A, 0x01, 0x02, 0x03, 0x04 };
    if (cmp_buf(buf, cbor_writer_used(&w), want, sizeof(want), "uint u32")) return 1;
    return 0;
}

TEST_CASE(text_string_short)
{
    uint8_t buf[8];
    struct cbor_writer w;
    cbor_writer_init(&w, buf, sizeof(buf));
    cbor_writer_text_cstr(&w, "ab");
    /* "ab" = 0x62 0x61 0x62 — major type 3, len 2, then bytes. */
    const uint8_t want[] = { 0x62, 'a', 'b' };
    if (cmp_buf(buf, cbor_writer_used(&w), want, sizeof(want), "text ab")) return 1;
    return 0;
}

TEST_CASE(bytes_empty)
{
    uint8_t buf[4];
    struct cbor_writer w;
    cbor_writer_init(&w, buf, sizeof(buf));
    cbor_writer_bytes(&w, NULL, 0);
    const uint8_t want[] = { 0x40 };
    if (cmp_buf(buf, cbor_writer_used(&w), want, sizeof(want), "bytes empty")) return 1;
    return 0;
}

TEST_CASE(array_then_elements)
{
    uint8_t buf[16];
    struct cbor_writer w;
    cbor_writer_init(&w, buf, sizeof(buf));
    cbor_writer_array(&w, 3);
    cbor_writer_uint(&w, 1);
    cbor_writer_uint(&w, 2);
    cbor_writer_uint(&w, 3);
    /* RFC 8949 A example: [1,2,3] = 83 01 02 03 */
    const uint8_t want[] = { 0x83, 0x01, 0x02, 0x03 };
    if (cmp_buf(buf, cbor_writer_used(&w), want, sizeof(want), "array 3")) return 1;
    return 0;
}

TEST_CASE(map_with_text_and_uint)
{
    uint8_t buf[32];
    struct cbor_writer w;
    cbor_writer_init(&w, buf, sizeof(buf));
    cbor_writer_map(&w, 2);
    cbor_writer_kv_text(&w, "a", "x");
    cbor_writer_kv_uint(&w, "b", 5);
    /* {"a":"x","b":5} = a2 61 61 61 78 61 62 05 */
    const uint8_t want[] = { 0xA2, 0x61, 'a', 0x61, 'x',
                                   0x61, 'b', 0x05 };
    if (cmp_buf(buf, cbor_writer_used(&w), want, sizeof(want), "map 2")) return 1;
    return 0;
}

TEST_CASE(overflow_sets_sticky_error)
{
    uint8_t buf[2];
    struct cbor_writer w;
    cbor_writer_init(&w, buf, sizeof(buf));
    cbor_writer_uint(&w, 0x12345678u);   /* 5 bytes — won't fit. */
    ASSERT_FALSE(cbor_writer_ok(&w));

    /* Further writes are no-ops once the error flag is sticky. */
    cbor_writer_uint(&w, 1);
    ASSERT_FALSE(cbor_writer_ok(&w));
    return 0;
}

TEST_CASE(bool_and_null)
{
    uint8_t buf[4];
    struct cbor_writer w;
    cbor_writer_init(&w, buf, sizeof(buf));
    cbor_writer_bool(&w, true);
    cbor_writer_bool(&w, false);
    cbor_writer_null(&w);
    const uint8_t want[] = { 0xF5, 0xF4, 0xF6 };
    if (cmp_buf(buf, cbor_writer_used(&w), want, sizeof(want), "simple")) return 1;
    return 0;
}

int main(void)
{
    fprintf(stdout, "test_cbor\n");
    RUN(uint_zero_is_single_byte);
    RUN(uint_24_uses_one_byte_extension);
    RUN(uint_big_endian_extension);
    RUN(text_string_short);
    RUN(bytes_empty);
    RUN(array_then_elements);
    RUN(map_with_text_and_uint);
    RUN(overflow_sets_sticky_error);
    RUN(bool_and_null);
    fprintf(stdout, "PASS\n");
    return 0;
}
