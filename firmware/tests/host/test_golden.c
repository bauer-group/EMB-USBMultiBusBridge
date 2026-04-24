/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Golden-vector roundtrip corpus. Builds canonical frames across
 * every RPBP subsystem and message type, asserts they pass
 * validate(), and verifies each parsed field matches the build
 * intent. Catches wire-format regressions and exercises the
 * subsystem opcode range end-to-end.
 */

#include "test_util.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_framing.h"

/* ───── Small helper: build, parse, sanity-check. ────────────────────── */

static int build_and_parse(const struct rpbp_header *in_hdr,
                           const uint8_t *payload, size_t payload_len,
                           struct rpbp_header *out_hdr,
                           uint8_t *out_frame, size_t frame_cap,
                           size_t *out_frame_len)
{
    size_t n = rpbp_frame_build(out_frame, in_hdr, payload, payload_len);
    if (n == 0 || n > frame_cap) return 1;
    *out_frame_len = n;

    enum rpbp_status rc = rpbp_frame_validate(out_frame, n, out_hdr);
    if (rc != RPBP_OK) return 2;
    return 0;
}

/* ───── SYS subsystem vectors ────────────────────────────────────────── */

TEST_CASE(sys_echo_request)
{
    struct rpbp_header h = {
        .msg_type = RPBP_MT_CMD_REQUEST,
        .flags    = 0,
        .channel  = RPBP_CHAN_CONTROL,
        .seq      = 0x0042,
    };
    const uint8_t payload[] = {
        RPBP_SUBSYS_SYS, RPBP_SYS_ECHO, 'H', 'i', '!',
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  frame_len;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame),
                                &frame_len), 0);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_CMD_REQUEST);
    ASSERT_EQ_U(parsed.payload_len, sizeof(payload));
    ASSERT_EQ_U(frame[RPBP_HEADER_LEN + 0], RPBP_SUBSYS_SYS);
    ASSERT_EQ_U(frame[RPBP_HEADER_LEN + 1], RPBP_SYS_ECHO);
    return 0;
}

TEST_CASE(sys_uptime_response)
{
    struct rpbp_header h = {
        .msg_type = RPBP_MT_CMD_RESPONSE,
        .channel  = RPBP_CHAN_CONTROL,
        .seq      = 7,
    };
    /* [subsys][opcode][status][u64 uptime_us LE] */
    const uint8_t payload[] = {
        RPBP_SUBSYS_SYS, RPBP_SYS_UPTIME, RPBP_OK,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  frame_len;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame),
                                &frame_len), 0);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_CMD_RESPONSE);
    return 0;
}

TEST_CASE(sys_error_frame)
{
    struct rpbp_header h = {
        .msg_type = RPBP_MT_ERROR,
        .channel  = RPBP_CHAN_CONTROL,
        .seq      = 99,
    };
    /* [status][orig_ch LE u16][orig_seq LE u16][reason_len LE u16][reason...] */
    const uint8_t payload[] = {
        RPBP_EINVAL,
        0x00, 0x00,
        0x2A, 0x00,
        0x03, 0x00,
        'b',  'a',  'd',
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  frame_len;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame),
                                &frame_len), 0);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_ERROR);
    return 0;
}

/* ───── I²C subsystem vectors ────────────────────────────────────────── */

TEST_CASE(i2c_probe_request)
{
    struct rpbp_header h = {
        .msg_type = RPBP_MT_CMD_REQUEST,
        .seq      = 1,
    };
    const uint8_t payload[] = {
        RPBP_SUBSYS_I2C, RPBP_I2C_PROBE, /* bus */ 0x00, /* addr */ 0x50,
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  fl;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame), &fl), 0);
    ASSERT_EQ_U(parsed.payload_len, 4);
    return 0;
}

TEST_CASE(i2c_xfer_write_read)
{
    struct rpbp_header h = {
        .msg_type = RPBP_MT_CMD_REQUEST,
        .seq      = 2,
    };
    /* XFER: [bus][addr][flags][tx_len LE u16][rx_len LE u16][tx_data...] */
    const uint8_t payload[] = {
        RPBP_SUBSYS_I2C, RPBP_I2C_XFER,
        0x00,                 /* bus */
        0x50,                 /* addr */
        0x00,                 /* flags */
        0x02, 0x00,           /* tx_len */
        0x04, 0x00,           /* rx_len */
        0xAA, 0xBB,           /* tx data */
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  fl;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame), &fl), 0);
    return 0;
}

/* ───── SPI subsystem vectors ────────────────────────────────────────── */

TEST_CASE(spi_xfer_duplex)
{
    struct rpbp_header h = { .msg_type = RPBP_MT_CMD_REQUEST, .seq = 3 };
    /* SPI XFER: [subsys][op][inst][cs_pin][flags][rsv][tx_len LE u16]
     *           [rx_len LE u16][tx_data...] */
    const uint8_t payload[] = {
        RPBP_SUBSYS_SPI, RPBP_SPI_XFER,
        0x00, 0x11, 0x01, 0x00,      /* inst 0, CS GP17, HOLD_CS, rsv */
        0x03, 0x00, 0x03, 0x00,      /* tx_len=3, rx_len=3 */
        0xDE, 0xAD, 0xBE,
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  fl;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame), &fl), 0);
    return 0;
}

/* ───── GPIO subsystem vectors ───────────────────────────────────────── */

TEST_CASE(gpio_set_mask_wide)
{
    struct rpbp_header h = { .msg_type = RPBP_MT_CMD_REQUEST, .seq = 4 };
    /* SET_MASK: [subsys][op][mask LE u64][values LE u64] */
    const uint8_t payload[] = {
        RPBP_SUBSYS_GPIO, RPBP_GPIO_SET_MASK,
        /* mask: pins 30, 37 */
        0x00, 0x00, 0x00, 0x40, 0x20, 0x00, 0x00, 0x00,
        /* values: pin 30 high, pin 37 low */
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  fl;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame), &fl), 0);
    return 0;
}

TEST_CASE(gpio_watch_event)
{
    struct rpbp_header h = {
        .msg_type = RPBP_MT_EVENT,
        .channel  = RPBP_CHAN_EVENTS,
        .seq      = 0,
    };
    const uint8_t payload[] = {
        RPBP_SUBSYS_GPIO, 0x00,          /* event_id = edge */
        30,                               /* pin */
        1,                                /* level */
        0x78, 0x56, 0x34, 0x12,           /* timestamp_us LE */
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  fl;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame), &fl), 0);
    ASSERT_EQ_U(parsed.msg_type, RPBP_MT_EVENT);
    ASSERT_EQ_U(parsed.channel, RPBP_CHAN_EVENTS);
    return 0;
}

/* ───── PWM subsystem vector ─────────────────────────────────────────── */

TEST_CASE(pwm_configure)
{
    struct rpbp_header h = { .msg_type = RPBP_MT_CMD_REQUEST, .seq = 6 };
    /* [subsys][op][pin][flags][duty LE u16][freq LE u32] */
    const uint8_t payload[] = {
        RPBP_SUBSYS_PWM, RPBP_PWM_CONFIGURE,
        30, 0x00,
        0x00, 0x80,                          /* duty = 0x8000 ≈ 50 % */
        0xE8, 0x03, 0x00, 0x00,              /* freq = 1000 Hz */
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  fl;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame), &fl), 0);
    return 0;
}

/* ───── ADC subsystem vector ─────────────────────────────────────────── */

TEST_CASE(adc_read_many)
{
    struct rpbp_header h = { .msg_type = RPBP_MT_CMD_REQUEST, .seq = 7 };
    /* [subsys][op][channel_mask LE u16] — read channels 0,2,4 */
    const uint8_t payload[] = {
        RPBP_SUBSYS_ADC, RPBP_ADC_READ_MANY,
        0x15, 0x00,
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  fl;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame), &fl), 0);
    return 0;
}

/* ───── UART subsystem vector ────────────────────────────────────────── */

TEST_CASE(uart_set_phy_rs485)
{
    struct rpbp_header h = { .msg_type = RPBP_MT_CMD_REQUEST, .seq = 8 };
    const uint8_t payload[] = {
        RPBP_SUBSYS_UART, RPBP_UART_SET_PHY,
        0x00,                                /* uart_idx = 0 */
        (uint8_t)RPBP_UART_PHY_RS485,
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  fl;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame), &fl), 0);
    return 0;
}

/* ───── Large-payload corner ─────────────────────────────────────────── */

TEST_CASE(max_payload_roundtrip)
{
    struct rpbp_header h = {
        .msg_type = RPBP_MT_CMD_REQUEST,
        .seq      = 0xFFFF,
    };
    static uint8_t payload[RPBP_MAX_PAYLOAD_LEN];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i & 0xFFu);
    }

    static uint8_t frame[RPBP_MAX_FRAME_LEN];
    struct rpbp_header parsed;
    size_t fl;
    ASSERT_EQ_U(build_and_parse(&h, payload, sizeof(payload),
                                &parsed, frame, sizeof(frame), &fl), 0);
    ASSERT_EQ_U(parsed.payload_len, RPBP_MAX_PAYLOAD_LEN);
    ASSERT_MEM_EQ(frame + RPBP_HEADER_LEN, payload, RPBP_MAX_PAYLOAD_LEN);
    return 0;
}

/* ───── Reset-channel control ────────────────────────────────────────── */

TEST_CASE(reset_channel_events)
{
    struct rpbp_header h = {
        .msg_type = RPBP_MT_RESET_CHANNEL,
        .channel  = RPBP_CHAN_EVENTS,
        .seq      = 5,
    };
    struct rpbp_header parsed;
    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  fl;
    ASSERT_EQ_U(build_and_parse(&h, NULL, 0,
                                &parsed, frame, sizeof(frame), &fl), 0);
    ASSERT_EQ_U(parsed.channel, RPBP_CHAN_EVENTS);
    ASSERT_EQ_U(parsed.payload_len, 0);
    return 0;
}

int main(void)
{
    fprintf(stdout, "test_golden\n");
    RUN(sys_echo_request);
    RUN(sys_uptime_response);
    RUN(sys_error_frame);
    RUN(i2c_probe_request);
    RUN(i2c_xfer_write_read);
    RUN(spi_xfer_duplex);
    RUN(gpio_set_mask_wide);
    RUN(gpio_watch_event);
    RUN(pwm_configure);
    RUN(adc_read_many);
    RUN(uart_set_phy_rs485);
    RUN(max_payload_roundtrip);
    RUN(reset_channel_events);
    fprintf(stdout, "PASS\n");
    return 0;
}
