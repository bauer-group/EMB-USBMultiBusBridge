/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * DMX subsystem dispatcher (subsys = 8). Wire format in
 * docs/protocol/subsystems/dmx.md.
 */

#include "cmd_dmx.h"

#include "peripherals/dmx.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#include <string.h>

static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
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
        rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, RPBP_DMX_GET_INFO,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    struct dmx_info info;
    dmx_get_info(&info);

    /* Reply: count u8 | pin u8 | running u8 | reserved u8
     *      | slot_count LE u16 | break_us LE u16 | mab_us LE u16 = 10 B. */
    uint8_t buf[10];
    buf[0] = info.count;
    buf[1] = info.pin;
    buf[2] = info.running;
    buf[3] = 0;
    wr_le16(&buf[4], info.slot_count);
    wr_le16(&buf[6], info.break_us);
    wr_le16(&buf[8], info.mab_us);
    rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, RPBP_DMX_GET_INFO, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x01 — SET_SLOTS ─────────────────────────────────────────────── */

static enum rpbp_status op_set_slots(const struct rpbp_header *req,
                                     const uint8_t *args, size_t args_len)
{
    /* [idx u8][offset LE u16][bytes …] — minimum 3 B header. */
    if (args_len < 3u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, RPBP_DMX_SET_SLOTS,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t  idx    = args[0];
    uint16_t offset = rd_le16(&args[1]);
    const uint8_t *body = args + 3;
    size_t body_len = args_len - 3u;

    if (idx != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, RPBP_DMX_SET_SLOTS,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = dmx_set_slots(offset, body, body_len);
    rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, RPBP_DMX_SET_SLOTS,
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
        rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, opcode,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, opcode,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = (opcode == RPBP_DMX_START) ? dmx_start() : dmx_stop();
    rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, opcode,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x04 — SET_BREAK ─────────────────────────────────────────────── */

static enum rpbp_status op_set_break(const struct rpbp_header *req,
                                     const uint8_t *args, size_t args_len)
{
    /* [idx u8][break_us LE u16][mab_us LE u16] = 5 B. */
    if (args_len != 5u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, RPBP_DMX_SET_BREAK,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, RPBP_DMX_SET_BREAK,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    uint16_t break_us = rd_le16(&args[1]);
    uint16_t mab_us   = rd_le16(&args[3]);

    int rc = dmx_set_timing(break_us, mab_us);
    rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, RPBP_DMX_SET_BREAK,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_dmx_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor)
{
    (void)cbor;

    switch (opcode) {
    case RPBP_DMX_GET_INFO:  return op_get_info(req, args, args_len);
    case RPBP_DMX_SET_SLOTS: return op_set_slots(req, args, args_len);
    case RPBP_DMX_START:
    case RPBP_DMX_STOP:      return op_start_stop(req, opcode, args, args_len);
    case RPBP_DMX_SET_BREAK: return op_set_break(req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_DMX, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
