/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * LED subsystem dispatcher (subsys = 7). Wire format in
 * docs/protocol/subsystems/led.md.
 */

#include "cmd_led.h"

#include "peripherals/led_strip.h"
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
        rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_GET_INFO,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    struct led_strip_info info;
    led_strip_get_info(&info);

    /* Reply: count u8 | pin u8 | format u8 | reserved u8
     *      | pixel_count LE u16 | max_pixels LE u16 = 8 B. */
    uint8_t buf[8];
    buf[0] = info.count;
    buf[1] = info.pin;
    buf[2] = info.format;
    buf[3] = 0;
    wr_le16(&buf[4], info.pixel_count);
    wr_le16(&buf[6], info.max_pixels);
    rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_GET_INFO, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x01 — CONFIGURE ─────────────────────────────────────────────── */

static enum rpbp_status op_configure(const struct rpbp_header *req,
                                     const uint8_t *args, size_t args_len)
{
    /* [idx u8][format u8][pixel_count LE u16] = 4 B. */
    if (args_len != 4u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_CONFIGURE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t  idx          = args[0];
    uint8_t  format       = args[1];
    uint16_t pixel_count  = rd_le16(&args[2]);

    if (idx != 0u) {
        /* v1 supports a single strip (idx 0). Future revisions might
         * add a second instance on PIO2 SM 1 once DMX moves elsewhere
         * — keep the namespace open by rejecting non-zero idx with
         * ENOENT instead of EINVAL. */
        rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_CONFIGURE,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    if (format > (uint8_t)RPBP_LED_FMT_GRBW) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_CONFIGURE,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = led_strip_configure(pixel_count,
                                 (enum led_strip_format)format);
    rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_CONFIGURE,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x02 — WRITE ─────────────────────────────────────────────────── */

static enum rpbp_status op_write(const struct rpbp_header *req,
                                 const uint8_t *args, size_t args_len)
{
    /* [idx u8][offset LE u16][bytes …] — minimum 3 B header. */
    if (args_len < 3u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_WRITE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t  idx    = args[0];
    uint16_t offset = rd_le16(&args[1]);
    const uint8_t *body = args + 3;
    size_t body_len = args_len - 3u;

    if (idx != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_WRITE,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = led_strip_write(offset, body, body_len);
    rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_WRITE,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x03 — CLEAR ─────────────────────────────────────────────────── */

static enum rpbp_status op_clear(const struct rpbp_header *req,
                                 const uint8_t *args, size_t args_len)
{
    /* [idx u8] = 1 B. */
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_CLEAR,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (args[0] != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_CLEAR,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = led_strip_clear();
    rpbp_reply_cmd(req, RPBP_SUBSYS_LED, RPBP_LED_CLEAR,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_led_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor)
{
    (void)cbor;

    switch (opcode) {
    case RPBP_LED_GET_INFO:  return op_get_info (req, args, args_len);
    case RPBP_LED_CONFIGURE: return op_configure(req, args, args_len);
    case RPBP_LED_WRITE:     return op_write    (req, args, args_len);
    case RPBP_LED_CLEAR:     return op_clear    (req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_LED, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
