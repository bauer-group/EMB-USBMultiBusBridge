/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * 1-Wire subsystem dispatcher (subsys = 9). Wire format in
 * docs/protocol/subsystems/onewire.md.
 */

#include "cmd_onewire.h"

#include "peripherals/onewire.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#include <string.h>

static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
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
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_GET_INFO,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    struct onewire_info info;
    onewire_get_info(&info);

    /* Reply: count u8 | pin u8 | speed u8 | reserved u8 = 4 B. */
    uint8_t buf[4];
    buf[0] = info.count;
    buf[1] = info.pin;
    buf[2] = info.speed;
    buf[3] = info.reserved;
    rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_GET_INFO, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x01 — RESET ─────────────────────────────────────────────────── */

static enum rpbp_status op_reset(const struct rpbp_header *req,
                                 const uint8_t *args, size_t args_len)
{
    /* [idx u8] = 1 B. */
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_RESET,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_RESET,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = onewire_reset_present();
    if (rc < 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_RESET,
                       rc_to_status(rc), NULL, 0, false);
        return RPBP_OK;
    }
    /* Reply: presence u8 (0 = no slaves, 1 = at least one). */
    uint8_t buf[1] = { (uint8_t)(rc & 1) };
    rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_RESET, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x02 — WRITE ─────────────────────────────────────────────────── */

static enum rpbp_status op_write(const struct rpbp_header *req,
                                 const uint8_t *args, size_t args_len)
{
    /* [idx u8][bytes …] — minimum 1 B header. */
    if (args_len < 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_WRITE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_WRITE,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = onewire_write(args + 1, args_len - 1u);
    rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_WRITE,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x03 — READ ──────────────────────────────────────────────────── */

#define ONEWIRE_READ_MAX  256u   /* hard cap on a single-shot read */

static enum rpbp_status op_read(const struct rpbp_header *req,
                                const uint8_t *args, size_t args_len)
{
    /* [idx u8][len LE u16] = 3 B. */
    if (args_len != 3u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_READ,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_READ,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    uint16_t len = rd_le16(&args[1]);
    if (len == 0u || len > ONEWIRE_READ_MAX) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_READ,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t buf[ONEWIRE_READ_MAX];
    int rc = onewire_read(buf, len);
    if (rc < 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_READ,
                       rc_to_status(rc), NULL, 0, false);
        return RPBP_OK;
    }
    rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_READ, RPBP_OK,
                   buf, len, false);
    return RPBP_OK;
}

/* ───── 0x04 — TRIPLET ───────────────────────────────────────────────── */

static enum rpbp_status op_triplet(const struct rpbp_header *req,
                                   const uint8_t *args, size_t args_len)
{
    /* [idx u8][dir u8] = 2 B. */
    if (args_len != 2u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_TRIPLET,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_TRIPLET,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = onewire_triplet(args[1]);
    if (rc < 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_TRIPLET,
                       rc_to_status(rc), NULL, 0, false);
        return RPBP_OK;
    }
    /* Reply: triplet u8 — bit 0 id, bit 1 cmp, bit 2 dir. */
    uint8_t buf[1] = { (uint8_t)(rc & 0x07) };
    rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_TRIPLET, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_onewire_dispatch(const struct rpbp_header *req,
                                      uint8_t opcode,
                                      const uint8_t *args, size_t args_len,
                                      bool cbor)
{
    (void)cbor;

    switch (opcode) {
    case RPBP_ONEWIRE_GET_INFO: return op_get_info(req, args, args_len);
    case RPBP_ONEWIRE_RESET:    return op_reset   (req, args, args_len);
    case RPBP_ONEWIRE_WRITE:    return op_write   (req, args, args_len);
    case RPBP_ONEWIRE_READ:     return op_read    (req, args, args_len);
    case RPBP_ONEWIRE_TRIPLET:  return op_triplet (req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_ONEWIRE, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
