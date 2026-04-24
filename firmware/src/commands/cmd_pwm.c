/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * PWM subsystem dispatcher. Wire format in
 * docs/protocol/subsystems/pwm.md.
 */

#include "cmd_pwm.h"

#include "peripherals/pwm.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#include <string.h>

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
    p[0] = (uint8_t)(v         & 0xFFu);
    p[1] = (uint8_t)((v >> 8)  & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint8_t rc_to_status(int rc)
{
    if (rc >= 0) return RPBP_OK;
    int v = -rc;
    if (v <= 0 || v > 0xFF) return RPBP_EIO;
    return (uint8_t)v;
}

/* ───── 0x00 — CONFIGURE ─────────────────────────────────────────────── */

static enum rpbp_status op_configure(const struct rpbp_header *req,
                                     const uint8_t *args, size_t args_len)
{
    /* [pin][flags][duty_q16 LE u16][freq_hz LE u32] = 8 bytes. */
    if (args_len != 8u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, RPBP_PWM_CONFIGURE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t  pin      = args[0];
    uint8_t  flags    = args[1];
    uint16_t duty_q16 = rd_le16(&args[2]);
    uint32_t freq_hz  = rd_le32(&args[4]);

    if ((flags & ~RPBP_PWM_FLAG_INVERT) != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, RPBP_PWM_CONFIGURE,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }
    bool invert = (flags & RPBP_PWM_FLAG_INVERT) != 0;

    int rc = pwm_hw_configure(pin, freq_hz, duty_q16, invert);
    rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, RPBP_PWM_CONFIGURE,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x01 — SET_DUTY ──────────────────────────────────────────────── */

static enum rpbp_status op_set_duty(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    if (args_len != 3u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, RPBP_PWM_SET_DUTY,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t  pin      = args[0];
    uint16_t duty_q16 = rd_le16(&args[1]);

    int rc = pwm_hw_set_duty(pin, duty_q16);
    rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, RPBP_PWM_SET_DUTY,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x02 / 0x03 — START / STOP ───────────────────────────────────── */

static enum rpbp_status op_start_stop(const struct rpbp_header *req,
                                      uint8_t opcode,
                                      const uint8_t *args, size_t args_len)
{
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, opcode,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = (opcode == RPBP_PWM_START) ? pwm_hw_start(args[0])
                                        : pwm_hw_stop (args[0]);
    rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, opcode,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x04 — GET_STATE ─────────────────────────────────────────────── */

static enum rpbp_status op_get_state(const struct rpbp_header *req,
                                     const uint8_t *args, size_t args_len)
{
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, RPBP_PWM_GET_STATE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    struct pwm_hw_state st;
    int rc = pwm_hw_state(args[0], &st);
    if (rc < 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, RPBP_PWM_GET_STATE,
                       rc_to_status(rc), NULL, 0, false);
        return RPBP_OK;
    }

    /* Reply: freq_hz u32 | duty_q16 u16 | running u8 | invert u8
     *         | slice u8 | channel u8 = 10 bytes. */
    uint8_t buf[10];
    wr_le32(&buf[0], st.freq_hz);
    wr_le16(&buf[4], st.duty_q16);
    buf[6] = st.running;
    buf[7] = st.invert;
    buf[8] = st.slice;
    buf[9] = st.channel;

    rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, RPBP_PWM_GET_STATE, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_pwm_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor)
{
    (void)cbor;

    switch (opcode) {
    case RPBP_PWM_CONFIGURE: return op_configure(req, args, args_len);
    case RPBP_PWM_SET_DUTY:  return op_set_duty (req, args, args_len);
    case RPBP_PWM_START:
    case RPBP_PWM_STOP:      return op_start_stop(req, opcode, args, args_len);
    case RPBP_PWM_GET_STATE: return op_get_state (req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_PWM, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
