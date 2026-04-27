/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * SPI subsystem dispatcher. Opcodes documented in
 * docs/protocol/subsystems/spi.md.
 */

#include "cmd_spi.h"

#include "peripherals/spi.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#include <string.h>

/* Bounds on a single XFER. tx_len + rx_len must fit the RPBP payload
 * (4 KB ceiling, minus a few bytes of command framing). Choose 2 KB
 * as a round cap — larger transfers segment on the host side. */
#define CMD_SPI_MAX_LEN   2048u

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

static uint8_t rc_to_status(int rc)
{
    if (rc >= 0) return RPBP_OK;
    int v = -rc;
    if (v <= 0 || v > 0xFF) return RPBP_EIO;
    return (uint8_t)v;
}

/* ───── 0x00 — XFER ──────────────────────────────────────────────────── */

static enum rpbp_status op_xfer(const struct rpbp_header *req,
                                const uint8_t *args, size_t args_len)
{
    /*
     * Request body:
     *   [0]      instance
     *   [1]      cs_pin  (0xFF = no CS manipulation)
     *   [2]      flags   (RPBP_SPI_FLAG_*)
     *   [3]      reserved (MUST be 0)
     *   [4..5]   tx_len  (LE u16, MUST match payload-tail length)
     *   [6..7]   rx_len  (LE u16)
     *   [8..]    tx_data (tx_len bytes)
     */
    if (args_len < 8u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_XFER,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    uint8_t  instance = args[0];
    uint8_t  cs_pin   = args[1];
    uint8_t  flags    = args[2];
    uint8_t  rsv      = args[3];
    uint16_t tx_len   = rd_le16(&args[4]);
    uint16_t rx_len   = rd_le16(&args[6]);

    if (rsv != 0u || (flags & ~RPBP_SPI_FLAG_HOLD_CS) != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_XFER,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }
    if (args_len != (size_t)8u + (size_t)tx_len) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_XFER,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    if (tx_len > CMD_SPI_MAX_LEN || rx_len > CMD_SPI_MAX_LEN) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_XFER,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    const uint8_t *tx = (tx_len > 0u) ? &args[8] : NULL;
    bool hold_cs      = (flags & RPBP_SPI_FLAG_HOLD_CS) != 0;

    /*
     * On the wire we run one full-duplex transfer of
     * `frames = max(tx_len, rx_len)` bytes: TX shorter than frames
     * gets zero padding, RX shorter gets the first `rx_len` bytes of
     * the MISO stream returned. This matches the libftdi2-over-SPI /
     * linux-spidev convention.
     */
    size_t frames = (tx_len > rx_len) ? tx_len : rx_len;

    static uint8_t tx_buf[CMD_SPI_MAX_LEN];
    static uint8_t rx_buf[CMD_SPI_MAX_LEN];

    if (frames == 0u) {
        /* Degenerate XFER: nothing to do. Still honour HOLD_CS / CS
         * assertion by poking cs_acquire so the pin is reserved. */
        (void)spi_hw_cs_acquire(cs_pin);
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_XFER, RPBP_OK,
                       NULL, 0, false);
        return RPBP_OK;
    }

    memset(tx_buf, 0, frames);
    if (tx_len > 0u && tx != NULL) {
        memcpy(tx_buf, tx, tx_len);
    }

    int rc = spi_hw_xfer(instance, cs_pin, hold_cs, tx_buf, rx_buf, frames);

    uint8_t  status     = rc_to_status(rc);
    uint16_t reply_rx   = (rc >= 0 && rx_len > 0u) ? rx_len : 0u;

    /* Reply body: [rx_len LE u16][rx_data]. */
    static uint8_t reply[2u + CMD_SPI_MAX_LEN];
    reply[0] = (uint8_t)(reply_rx & 0xFFu);
    reply[1] = (uint8_t)((reply_rx >> 8) & 0xFFu);
    if (reply_rx > 0u) {
        memcpy(&reply[2], rx_buf, reply_rx);
    }

    rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_XFER, status,
                   reply, (size_t)2u + (size_t)reply_rx, false);
    return RPBP_OK;
}

/* ───── 0x01 — SET_MODE ──────────────────────────────────────────────── */

static enum rpbp_status op_set_mode(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    if (args_len != 2u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_SET_MODE,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t instance = args[0];
    uint8_t mode_bits = args[1];

    int rc = spi_hw_set_mode(instance, mode_bits);
    rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_SET_MODE,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── 0x02 — SET_FREQ ──────────────────────────────────────────────── */

static enum rpbp_status op_set_freq(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    if (args_len != 5u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_SET_FREQ,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t  instance = args[0];
    uint32_t freq_hz  = rd_le32(&args[1]);

    uint32_t applied = spi_hw_set_freq(instance, freq_hz);
    if (applied == 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_SET_FREQ,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }

    /* Reply body: actual applied rate (LE u32). */
    uint8_t buf[4] = {
        (uint8_t)(applied         & 0xFFu),
        (uint8_t)((applied >> 8)  & 0xFFu),
        (uint8_t)((applied >> 16) & 0xFFu),
        (uint8_t)((applied >> 24) & 0xFFu),
    };
    rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_SET_FREQ, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x03 — GET_FREQ ──────────────────────────────────────────────── */

static enum rpbp_status op_get_freq(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_GET_FREQ,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t  instance = args[0];
    uint32_t freq_hz  = spi_hw_freq(instance);

    if (freq_hz == 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_GET_FREQ,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }

    uint8_t buf[4] = {
        (uint8_t)(freq_hz         & 0xFFu),
        (uint8_t)((freq_hz >> 8)  & 0xFFu),
        (uint8_t)((freq_hz >> 16) & 0xFFu),
        (uint8_t)((freq_hz >> 24) & 0xFFu),
    };
    rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, RPBP_SPI_GET_FREQ, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x04 / 0x05 — CS_ASSERT / CS_RELEASE ─────────────────────────── */

static enum rpbp_status op_cs(const struct rpbp_header *req,
                              uint8_t opcode,
                              const uint8_t *args, size_t args_len)
{
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, opcode,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t cs_pin = args[0];

    int rc = spi_hw_cs_acquire(cs_pin);
    if (rc == 0) {
        rc = (opcode == RPBP_SPI_CS_ASSERT) ? spi_hw_cs_assert(cs_pin)
                                            : spi_hw_cs_release(cs_pin);
    }
    rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, opcode,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_spi_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor)
{
    (void)cbor;

    switch (opcode) {
    case RPBP_SPI_XFER:       return op_xfer    (req, args, args_len);
    case RPBP_SPI_SET_MODE:   return op_set_mode(req, args, args_len);
    case RPBP_SPI_SET_FREQ:   return op_set_freq(req, args, args_len);
    case RPBP_SPI_GET_FREQ:   return op_get_freq(req, args, args_len);
    case RPBP_SPI_CS_ASSERT:
    case RPBP_SPI_CS_RELEASE: return op_cs      (req, opcode, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_SPI, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
