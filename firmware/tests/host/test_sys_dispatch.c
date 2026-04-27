/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * End-to-end dispatcher round-trip tests.
 *
 * Drives full RPBP frames through rpbp_dispatch_frame(), captures
 * every outgoing frame the dispatcher emits, and asserts the
 * captured bytes decode to the expected shape.
 *
 * Linked against:
 *   - rpbp_framing.c  (portable already)
 *   - rpbp_crc32c.c   (portable)
 *   - rpbp_cbor.c     (portable)
 *   - rpbp_dispatch.c (needs host_stubs for time_us_64 + cmd_*)
 *   - rpbp_stream.c   (uses the same stubs — session access)
 *   - host_stubs.c    (time_us_64 + subsystem dispatcher stand-ins)
 */

#include "test_util.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_framing.h"
#include "rpbp/rpbp_dispatch.h"

#include <stdint.h>

/* ───── TX capture ────────────────────────────────────────────────── */

#define CAPTURE_MAX_FRAMES  8
#define CAPTURE_MAX_BYTES   RPBP_MAX_FRAME_LEN

static struct {
    uint8_t  frames[CAPTURE_MAX_FRAMES][CAPTURE_MAX_BYTES];
    uint16_t frame_len[CAPTURE_MAX_FRAMES];
    size_t   count;
} g_cap;

static int capture_tx(const uint8_t *bytes, size_t len)
{
    if (g_cap.count >= CAPTURE_MAX_FRAMES) return -1;
    if (len > CAPTURE_MAX_BYTES)            return -1;
    memcpy(g_cap.frames[g_cap.count], bytes, len);
    g_cap.frame_len[g_cap.count] = (uint16_t)len;
    g_cap.count++;
    return 0;
}

static void cap_reset(void)
{
    g_cap.count = 0;
    memset(g_cap.frame_len, 0, sizeof(g_cap.frame_len));
}

/* Fetch a validated header from the Nth captured frame. */
static int cap_header(size_t idx, struct rpbp_header *out)
{
    if (idx >= g_cap.count) return -1;
    return (int)rpbp_frame_validate(g_cap.frames[idx],
                                    g_cap.frame_len[idx], out);
}

/* ───── Frame-builder helpers ─────────────────────────────────────── */

/* Build a well-formed frame directly into `dst`; fill in header fields
 * the caller cares about and zero the rest. */
static size_t make_frame(uint8_t *dst, uint8_t msg_type, uint16_t channel,
                         uint16_t seq, const uint8_t *payload, size_t plen)
{
    struct rpbp_header h = {
        .msg_type     = msg_type,
        .flags        = 0,
        .channel      = channel,
        .seq          = seq,
        .timestamp_us = 0u,
    };
    return rpbp_frame_build(dst, &h, payload, plen);
}

static enum rpbp_status drive(const uint8_t *frame, size_t flen)
{
    /* Validate + deliver — the transport would normally do this. */
    struct rpbp_header h;
    enum rpbp_status rc = rpbp_frame_validate(frame, flen, &h);
    if (rc != RPBP_OK) return rc;
    const uint8_t *payload = &frame[RPBP_HEADER_LEN];
    return rpbp_dispatch_frame(&h, payload, h.payload_len);
}

/* ───── Tests ─────────────────────────────────────────────────────── */

TEST_CASE(hello_opens_session_and_echoes_hello)
{
    cap_reset();
    rpbp_dispatch_init(capture_tx);

    uint8_t f[RPBP_MAX_FRAME_LEN];
    size_t  n = make_frame(f, RPBP_MT_HELLO, RPBP_CHAN_CONTROL, 0, NULL, 0);
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_U(drive(f, n), RPBP_OK);

    ASSERT_EQ_U(g_cap.count, 1);
    struct rpbp_header parsed;
    ASSERT_EQ_U(cap_header(0, &parsed), RPBP_OK);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_HELLO);
    return 0;
}

TEST_CASE(ping_returns_pong_with_t2_payload)
{
    cap_reset();
    rpbp_dispatch_init(capture_tx);

    /* Open session via HELLO first. */
    uint8_t f[RPBP_MAX_FRAME_LEN];
    size_t  n = make_frame(f, RPBP_MT_HELLO, 0, 0, NULL, 0);
    ASSERT_EQ_U(drive(f, n), RPBP_OK);

    cap_reset();

    n = make_frame(f, RPBP_MT_PING, RPBP_CHAN_CONTROL, 7, NULL, 0);
    ASSERT_EQ_U(drive(f, n), RPBP_OK);

    ASSERT_EQ_U(g_cap.count, 1);
    struct rpbp_header parsed;
    ASSERT_EQ_U(cap_header(0, &parsed), RPBP_OK);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_PONG);
    ASSERT_EQ_U(parsed.seq,      7);
    ASSERT_EQ_U(parsed.payload_len, 4);  /* t2_dev_rx_us */
    return 0;
}

TEST_CASE(cmd_request_before_hello_returns_eperm_error)
{
    cap_reset();
    rpbp_dispatch_init(capture_tx);

    uint8_t args[] = { RPBP_SUBSYS_SYS, RPBP_SYS_ECHO };
    uint8_t f[RPBP_MAX_FRAME_LEN];
    size_t  n = make_frame(f, RPBP_MT_CMD_REQUEST, 0, 1, args, sizeof(args));
    ASSERT_EQ_U(drive(f, n), RPBP_OK);

    ASSERT_EQ_U(g_cap.count, 1);
    struct rpbp_header parsed;
    ASSERT_EQ_U(cap_header(0, &parsed), RPBP_OK);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_ERROR);
    /* First payload byte is the status code. */
    ASSERT_EQ_U(g_cap.frames[0][RPBP_HEADER_LEN], RPBP_EPERM);
    return 0;
}

TEST_CASE(cmd_request_routes_to_subsystem_stub)
{
    cap_reset();
    rpbp_dispatch_init(capture_tx);

    /* Open the session. */
    uint8_t f[RPBP_MAX_FRAME_LEN];
    size_t  n = make_frame(f, RPBP_MT_HELLO, 0, 0, NULL, 0);
    ASSERT_EQ_U(drive(f, n), RPBP_OK);
    cap_reset();

    /* Drive a CMD_REQUEST targeting I2C — stub echoes OK. */
    uint8_t args[] = { RPBP_SUBSYS_I2C, RPBP_I2C_PROBE, 0x00, 0x50 };
    n = make_frame(f, RPBP_MT_CMD_REQUEST, 0, 4, args, sizeof(args));
    ASSERT_EQ_U(drive(f, n), RPBP_OK);

    ASSERT_EQ_U(g_cap.count, 1);
    struct rpbp_header parsed;
    ASSERT_EQ_U(cap_header(0, &parsed), RPBP_OK);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_CMD_RESPONSE);
    /* Binary response layout: [subsys][opcode][status]. */
    const uint8_t *pl = &g_cap.frames[0][RPBP_HEADER_LEN];
    ASSERT_EQ_U(pl[0], RPBP_SUBSYS_I2C);
    ASSERT_EQ_U(pl[1], RPBP_I2C_PROBE);
    ASSERT_EQ_U(pl[2], RPBP_OK);
    return 0;
}

TEST_CASE(reset_channel_acks_on_control)
{
    cap_reset();
    rpbp_dispatch_init(capture_tx);

    uint8_t f[RPBP_MAX_FRAME_LEN];
    size_t  n = make_frame(f, RPBP_MT_RESET_CHANNEL, RPBP_CHAN_CONTROL, 3,
                           NULL, 0);
    ASSERT_EQ_U(drive(f, n), RPBP_OK);

    ASSERT_EQ_U(g_cap.count, 1);
    struct rpbp_header parsed;
    ASSERT_EQ_U(cap_header(0, &parsed), RPBP_OK);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_CMD_RESPONSE);
    ASSERT_EQ_U(parsed.seq, 3);
    return 0;
}

TEST_CASE(wrong_direction_frame_yields_error)
{
    cap_reset();
    rpbp_dispatch_init(capture_tx);

    uint8_t f[RPBP_MAX_FRAME_LEN];
    /* CMD_RESPONSE is device-to-host; host sending it is a protocol
     * violation. The dispatcher should emit an ERROR frame. */
    size_t  n = make_frame(f, RPBP_MT_CMD_RESPONSE, 0, 0, NULL, 0);
    ASSERT_EQ_U(drive(f, n), RPBP_OK);

    ASSERT_EQ_U(g_cap.count, 1);
    struct rpbp_header parsed;
    ASSERT_EQ_U(cap_header(0, &parsed), RPBP_OK);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_ERROR);
    ASSERT_EQ_U(g_cap.frames[0][RPBP_HEADER_LEN], RPBP_EPROTO);
    return 0;
}

TEST_CASE(unknown_msg_type_rejected)
{
    cap_reset();
    rpbp_dispatch_init(capture_tx);

    /* Build a frame with an unknown msg_type. Use rpbp_header_encode
     * directly since rpbp_frame_build sets magic/version too. */
    struct rpbp_header h = {
        .msg_type     = 0x7Fu,           /* unknown */
        .flags        = 0,
        .channel      = 0,
        .seq          = 0,
        .payload_len  = 0,
        .timestamp_us = 0u,
    };
    uint8_t f[RPBP_HEADER_LEN + RPBP_CRC_LEN];
    size_t  n = rpbp_frame_build(f, &h, NULL, 0);
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_U(drive(f, n), RPBP_OK);

    ASSERT_EQ_U(g_cap.count, 1);
    struct rpbp_header parsed;
    ASSERT_EQ_U(cap_header(0, &parsed), RPBP_OK);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_ERROR);
    return 0;
}

int main(void)
{
    fprintf(stdout, "test_sys_dispatch\n");
    RUN(hello_opens_session_and_echoes_hello);
    RUN(ping_returns_pong_with_t2_payload);
    RUN(cmd_request_before_hello_returns_eperm_error);
    RUN(cmd_request_routes_to_subsystem_stub);
    RUN(reset_channel_acks_on_control);
    RUN(wrong_direction_frame_yields_error);
    RUN(unknown_msg_type_rejected);
    fprintf(stdout, "PASS\n");
    return 0;
}
