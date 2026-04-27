/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * RPBP v1 framing tests — encode/parse roundtrip, CRC check, rejection
 * of protocol violations.
 */

#include "test_util.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_framing.h"

static struct rpbp_header sample_header(void)
{
    struct rpbp_header h = {
        .magic        = RPBP_MAGIC,
        .version      = RPBP_VERSION,
        .msg_type     = RPBP_MT_PING,
        .flags        = RPBP_FL_CBOR,
        .channel      = RPBP_CHAN_CONTROL,
        .seq          = 42,
        .payload_len  = 0,
        .timestamp_us = 0x12345678u,
    };
    return h;
}

TEST_CASE(header_encode_decode_roundtrip)
{
    struct rpbp_header h = sample_header();
    uint8_t buf[RPBP_HEADER_LEN];
    rpbp_header_encode(&h, buf);

    /* Verify the on-wire byte layout — magic at offset 0, LE fields. */
    ASSERT_EQ_U(buf[0], RPBP_MAGIC);
    ASSERT_EQ_U(buf[1], RPBP_VERSION);
    ASSERT_EQ_U(buf[2], RPBP_MT_PING);
    ASSERT_EQ_U(buf[3], RPBP_FL_CBOR);
    ASSERT_EQ_U(buf[4], 0x00);              /* channel LE */
    ASSERT_EQ_U(buf[5], 0x00);
    ASSERT_EQ_U(buf[6], 42);                /* seq LE */
    ASSERT_EQ_U(buf[7], 0);
    ASSERT_EQ_U(buf[8], 0);                 /* payload_len LE */
    ASSERT_EQ_U(buf[12], 0x78);             /* timestamp_us LE */
    ASSERT_EQ_U(buf[13], 0x56);
    ASSERT_EQ_U(buf[14], 0x34);
    ASSERT_EQ_U(buf[15], 0x12);

    struct rpbp_header out;
    ASSERT_EQ_U(rpbp_header_parse(buf, &out), RPBP_OK);
    ASSERT_EQ_U(out.seq, 42);
    ASSERT_EQ_HEX(out.timestamp_us, 0x12345678u);
    return 0;
}

TEST_CASE(header_rejects_bad_magic)
{
    struct rpbp_header h = sample_header();
    uint8_t buf[RPBP_HEADER_LEN];
    rpbp_header_encode(&h, buf);
    buf[0] = 0x41;   /* 'A' — wrong. */

    struct rpbp_header out;
    ASSERT_EQ_U(rpbp_header_parse(buf, &out), RPBP_EPROTO);
    return 0;
}

TEST_CASE(header_rejects_bad_version)
{
    struct rpbp_header h = sample_header();
    uint8_t buf[RPBP_HEADER_LEN];
    rpbp_header_encode(&h, buf);
    buf[1] = 0x02;   /* future version — not accepted by v1 parser. */

    struct rpbp_header out;
    ASSERT_EQ_U(rpbp_header_parse(buf, &out), RPBP_EPROTO);
    return 0;
}

TEST_CASE(header_rejects_reserved_flag_bits)
{
    struct rpbp_header h = sample_header();
    uint8_t buf[RPBP_HEADER_LEN];
    rpbp_header_encode(&h, buf);
    buf[3] |= 0x80;   /* reserved bit 7 set. */

    struct rpbp_header out;
    ASSERT_EQ_U(rpbp_header_parse(buf, &out), RPBP_EPROTO);
    return 0;
}

TEST_CASE(header_rejects_fragment_plus_last)
{
    struct rpbp_header h = sample_header();
    uint8_t buf[RPBP_HEADER_LEN];
    rpbp_header_encode(&h, buf);
    buf[3] = RPBP_FL_FRAGMENT | RPBP_FL_LAST;

    struct rpbp_header out;
    ASSERT_EQ_U(rpbp_header_parse(buf, &out), RPBP_EPROTO);
    return 0;
}

TEST_CASE(header_rejects_oversize_payload)
{
    struct rpbp_header h = sample_header();
    h.payload_len = RPBP_MAX_PAYLOAD_LEN + 1;
    uint8_t buf[RPBP_HEADER_LEN];
    rpbp_header_encode(&h, buf);

    struct rpbp_header out;
    ASSERT_EQ_U(rpbp_header_parse(buf, &out), RPBP_EMSGSIZE);
    return 0;
}

TEST_CASE(frame_build_and_validate_roundtrip)
{
    struct rpbp_header h = sample_header();
    h.msg_type = RPBP_MT_CMD_REQUEST;
    const uint8_t payload[] = { 0x00, 0x01, 'h', 'e', 'l', 'l', 'o' };

    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  n = rpbp_frame_build(frame, &h, payload, sizeof(payload));
    ASSERT_EQ_U(n, RPBP_HEADER_LEN + sizeof(payload) + RPBP_CRC_LEN);

    /* payload_len in the header must have been filled in by the
     * build helper. */
    struct rpbp_header parsed;
    ASSERT_EQ_U(rpbp_frame_validate(frame, n, &parsed), RPBP_OK);
    ASSERT_EQ_U(parsed.payload_len, sizeof(payload));
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_CMD_REQUEST);
    ASSERT_MEM_EQ(frame + RPBP_HEADER_LEN, payload, sizeof(payload));
    return 0;
}

TEST_CASE(frame_validate_detects_bit_flip)
{
    struct rpbp_header h = sample_header();
    h.msg_type = RPBP_MT_CMD_REQUEST;
    const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };

    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  n = rpbp_frame_build(frame, &h, payload, sizeof(payload));

    /* flip one payload bit — CRC must now fail. */
    frame[RPBP_HEADER_LEN] ^= 0x01;

    struct rpbp_header parsed;
    ASSERT_EQ_U(rpbp_frame_validate(frame, n, &parsed), RPBP_ECRC);
    return 0;
}

TEST_CASE(frame_validate_detects_length_mismatch)
{
    struct rpbp_header h = sample_header();
    const uint8_t payload[4] = {0};

    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  n = rpbp_frame_build(frame, &h, payload, sizeof(payload));

    struct rpbp_header parsed;
    /* Claim a shorter frame than payload_len says. */
    ASSERT_EQ_U(rpbp_frame_validate(frame, n - 1, &parsed), RPBP_EMSGSIZE);
    return 0;
}

int main(void)
{
    fprintf(stdout, "test_framing\n");
    RUN(header_encode_decode_roundtrip);
    RUN(header_rejects_bad_magic);
    RUN(header_rejects_bad_version);
    RUN(header_rejects_reserved_flag_bits);
    RUN(header_rejects_fragment_plus_last);
    RUN(header_rejects_oversize_payload);
    RUN(frame_build_and_validate_roundtrip);
    RUN(frame_validate_detects_bit_flip);
    RUN(frame_validate_detects_length_mismatch);
    fprintf(stdout, "PASS\n");
    return 0;
}
