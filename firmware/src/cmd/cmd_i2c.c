/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * I²C subsystem dispatcher. Opcodes documented in
 * docs/protocol/subsystems/i2c.md.
 */

#include "cmd_i2c.h"

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"
#include "periph/i2c_hw.h"

#include <string.h>

/* ───── Common constants ─────────────────────────────────────────────── */

/* Per-transaction timeouts. Chosen generously relative to the
 * longest plausible standard / fast / fast-plus transaction so a
 * slow device is not cut off, yet short enough to keep the firmware
 * responsive to the watchdog (3 s). */
#define I2C_XFER_TIMEOUT_US      100000u    /* 100 ms */
#define I2C_PROBE_TIMEOUT_US       1000u    /*   1 ms */

/* Little-endian readers over request args. */
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

/*
 * Map a negative RPBP_* return from the i2c_hw layer onto the same
 * value we want in the wire status byte. Positive RPBP values fit
 * into u8; the negation is a simple arithmetic invert.
 */
static uint8_t rc_to_status(int rc)
{
    if (rc >= 0) return RPBP_OK;
    int v = -rc;
    if (v < 0 || v > 0xFF) return RPBP_EIO;
    return (uint8_t)v;
}

/* ───── 0x00 — PROBE ─────────────────────────────────────────────────── */

static enum rpbp_status op_probe(const struct rpbp_header *req,
                                 const uint8_t *args, size_t args_len)
{
    if (args_len != 2u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_PROBE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    uint8_t bus  = args[0];
    uint8_t addr = args[1];

    int rc = i2c_hw_probe(bus, addr, I2C_PROBE_TIMEOUT_US);
    uint8_t status = rc_to_status(rc);
    rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_PROBE, status,
                   NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x01 — XFER ──────────────────────────────────────────────────── */

static enum rpbp_status op_xfer(const struct rpbp_header *req,
                                const uint8_t *args, size_t args_len)
{
    /* Request body layout:
     *   [0]      bus
     *   [1]      addr
     *   [2]      flags
     *   [3..4]   tx_len (LE u16)
     *   [5..6]   rx_len (LE u16)
     *   [7..]    tx_data (tx_len bytes)
     */
    if (args_len < 7u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_XFER,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    uint8_t  bus     = args[0];
    uint8_t  addr    = args[1];
    uint8_t  flags   = args[2];
    uint16_t tx_len  = rd_le16(&args[3]);
    uint16_t rx_len  = rd_le16(&args[5]);

    if (args_len != (size_t)7u + (size_t)tx_len) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_XFER,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (tx_len > I2C_HW_MAX_BUFFER || rx_len > I2C_HW_MAX_BUFFER) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_XFER,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (flags & ~RPBP_I2C_FLAG_NO_STOP) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_XFER,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }

    const uint8_t *tx   = (tx_len > 0u) ? &args[7] : NULL;
    bool no_stop         = (flags & RPBP_I2C_FLAG_NO_STOP) != 0;

    /* Response body: [rx_len LE u16][rx_data]. Allocate on the stack
     * — 2 + I2C_HW_MAX_BUFFER = 2050 bytes, comfortably within the
     * 4 KB task stack. */
    static uint8_t result[2u + I2C_HW_MAX_BUFFER];
    uint8_t *rx_buf = (rx_len > 0u) ? &result[2] : NULL;

    int rc = i2c_hw_xfer(bus, addr, tx, tx_len, rx_buf, rx_len,
                         no_stop, I2C_XFER_TIMEOUT_US);

    uint16_t returned_rx = 0u;
    uint8_t  status      = rc_to_status(rc);
    if (rc >= 0) {
        returned_rx = rx_len;              /* i2c_hw_xfer reports total bytes; we know rx_len by contract */
    }

    result[0] = (uint8_t)(returned_rx & 0xFFu);
    result[1] = (uint8_t)((returned_rx >> 8) & 0xFFu);

    rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_XFER, status,
                   result, (size_t)2u + (size_t)returned_rx, false);
    return RPBP_OK;
}

/* ───── 0x02 — SCAN ──────────────────────────────────────────────────── */

static enum rpbp_status op_scan(const struct rpbp_header *req,
                                const uint8_t *args, size_t args_len)
{
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_SCAN,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    uint8_t bus = args[0];

    /* 128 addresses → 16 bytes bitmap. Bit set means slave ACK'd its
     * address. Bits for 0x00..0x07 and 0x78..0x7F are reserved by
     * the I²C spec; we still probe them so an out-of-spec device is
     * reported rather than hidden. */
    uint8_t bitmap[16] = {0};

    for (uint8_t addr = 0u; addr < 0x80u; ++addr) {
        int rc = i2c_hw_probe(bus, addr, I2C_PROBE_TIMEOUT_US);
        if (rc == 0) {
            bitmap[addr >> 3] |= (uint8_t)(1u << (addr & 0x7u));
        }
    }

    rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_SCAN, RPBP_OK,
                   bitmap, sizeof(bitmap), false);
    return RPBP_OK;
}

/* ───── 0x03 — SET_FREQ ──────────────────────────────────────────────── */

static enum rpbp_status op_set_freq(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    if (args_len != 5u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_SET_FREQ,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    uint8_t  bus      = args[0];
    uint32_t freq_hz  = rd_le32(&args[1]);

    int rc = i2c_hw_set_baudrate(bus, freq_hz);
    uint8_t status = (rc == 0) ? RPBP_OK : RPBP_EINVAL;
    rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_SET_FREQ, status,
                   NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x04 — GET_FREQ ──────────────────────────────────────────────── */

static enum rpbp_status op_get_freq(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_GET_FREQ,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    uint8_t  bus      = args[0];
    uint32_t freq_hz  = i2c_hw_baudrate(bus);

    if (freq_hz == 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_GET_FREQ,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }

    uint8_t buf[4] = {
        (uint8_t)(freq_hz         & 0xFFu),
        (uint8_t)((freq_hz >> 8)  & 0xFFu),
        (uint8_t)((freq_hz >> 16) & 0xFFu),
        (uint8_t)((freq_hz >> 24) & 0xFFu),
    };
    rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, RPBP_I2C_GET_FREQ, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_i2c_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor)
{
    (void)cbor;        /* I²C opcodes carry binary args in v1 */

    switch (opcode) {
    case RPBP_I2C_PROBE:    return op_probe   (req, args, args_len);
    case RPBP_I2C_XFER:     return op_xfer    (req, args, args_len);
    case RPBP_I2C_SCAN:     return op_scan    (req, args, args_len);
    case RPBP_I2C_SET_FREQ: return op_set_freq(req, args, args_len);
    case RPBP_I2C_GET_FREQ: return op_get_freq(req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_I2C, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
