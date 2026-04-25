/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * I²S subsystem dispatcher (subsys = 10). Wire format in
 * docs/protocol/subsystems/i2s.md.
 */

#include "cmd_i2s.h"

#include "peripherals/i2s.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#include <string.h>

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0]         |
           ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
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

/* ───── 0x00 — GET_INFO ──────────────────────────────────────────────── */

static enum rpbp_status op_get_info(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    (void)args;
    if (args_len != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, RPBP_I2S_GET_INFO,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    struct i2s_info info;
    i2s_get_info(&info);

    /* Reply: count u8 | bclk u8 | lrclk u8 | dout u8
     *      | sample_rate LE u32 | bit_depth u8 | running u8
     *      | reserved u8 | reserved u8 = 12 B. */
    uint8_t buf[12];
    buf[0] = info.count;
    buf[1] = info.bclk_pin;
    buf[2] = info.lrclk_pin;
    buf[3] = info.dout_pin;
    wr_le32(&buf[4], info.sample_rate_hz);
    buf[8]  = info.bit_depth;
    buf[9]  = info.running;
    buf[10] = 0;
    buf[11] = 0;
    rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, RPBP_I2S_GET_INFO, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x01 — CONFIGURE ─────────────────────────────────────────────── */

static enum rpbp_status op_configure(const struct rpbp_header *req,
                                     const uint8_t *args, size_t args_len)
{
    /* [idx u8][sample_rate LE u32] = 5 B. */
    if (args_len != 5u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, RPBP_I2S_CONFIGURE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, RPBP_I2S_CONFIGURE,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    uint32_t rate = rd_le32(&args[1]);
    int rc = i2s_configure(rate);
    rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, RPBP_I2S_CONFIGURE,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x02 / 0x03 — START / STOP ───────────────────────────────────── */

static enum rpbp_status op_start_stop(const struct rpbp_header *req,
                                      uint8_t opcode,
                                      const uint8_t *args, size_t args_len)
{
    /* [idx u8] = 1 B. */
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, opcode,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, opcode,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = (opcode == RPBP_I2S_START) ? i2s_start() : i2s_stop();
    rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, opcode,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x04 — WRITE ─────────────────────────────────────────────────── */

static enum rpbp_status op_write(const struct rpbp_header *req,
                                 const uint8_t *args, size_t args_len)
{
    /* [idx u8][stereo PCM frames …] — minimum 1 B header. */
    if (args_len < 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, RPBP_I2S_WRITE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, RPBP_I2S_WRITE,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = i2s_write(args + 1, args_len - 1u);
    rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, RPBP_I2S_WRITE,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_i2s_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor)
{
    (void)cbor;

    switch (opcode) {
    case RPBP_I2S_GET_INFO:  return op_get_info(req, args, args_len);
    case RPBP_I2S_CONFIGURE: return op_configure(req, args, args_len);
    case RPBP_I2S_START:
    case RPBP_I2S_STOP:      return op_start_stop(req, opcode, args, args_len);
    case RPBP_I2S_WRITE:     return op_write(req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2S, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
