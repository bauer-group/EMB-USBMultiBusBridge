/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * RPBP — RPBridge Protocol, v1.
 *
 * Authoritative spec: docs/protocol/rpbp-v1.md.
 *
 * Wire layout (little-endian throughout):
 *
 *     +------- 16 B header -------+----- 0..N payload -----+-- 4 B CRC --+
 *     | magic=0x52 | ver=0x01     |                        | CRC-32C     |
 *     | msg_type   | flags        |                        | Castagnoli  |
 *     | channel(2) | seq(2)       |                        | over        |
 *     | payload_len (4)           |                        | header      |
 *     | timestamp_us (4)          |                        | + payload   |
 *     +---------------------------+------------------------+-------------+
 *
 * Every enum / constant in this header is stable wire ABI — changes
 * require a MAJOR version bump.
 */

#ifndef RPBP_H
#define RPBP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ───── Wire constants ───────────────────────────────────────────────── */

#define RPBP_MAGIC              0x52u   /* 'R' */
#define RPBP_VERSION            0x01u
#define RPBP_HEADER_LEN         16u
#define RPBP_CRC_LEN            4u
#define RPBP_MAX_PAYLOAD_LEN    4096u
#define RPBP_MAX_FRAME_LEN      (RPBP_HEADER_LEN + RPBP_MAX_PAYLOAD_LEN + RPBP_CRC_LEN)

/*
 * Reassembly ceiling. A v1 receiver MUST handle at least 65 536 bytes
 * of reassembled payload; we set ours to that minimum. Larger targets
 * advertise a different value via the capability map.
 */
#define RPBP_MAX_REASSEMBLED_LEN    (64u * 1024u)

/* ───── Message types ────────────────────────────────────────────────── */

enum rpbp_msg_type {
    RPBP_MT_HELLO          = 0x00,
    RPBP_MT_CAPABILITIES   = 0x01,
    RPBP_MT_CMD_REQUEST    = 0x02,
    RPBP_MT_CMD_RESPONSE   = 0x03,
    RPBP_MT_STREAM_DATA    = 0x04,
    RPBP_MT_STREAM_CREDIT  = 0x05,
    RPBP_MT_EVENT          = 0x06,
    RPBP_MT_PING           = 0x07,
    RPBP_MT_PONG           = 0x08,
    RPBP_MT_ERROR          = 0x09,
    RPBP_MT_RESET_CHANNEL  = 0x0A,
    RPBP_MT_TIME_SYNC      = 0x0B,
};

/* ───── Header flags ─────────────────────────────────────────────────── */

#define RPBP_FL_CBOR            (1u << 0)
#define RPBP_FL_COMPRESSED      (1u << 1)
#define RPBP_FL_URGENT          (1u << 2)
#define RPBP_FL_FRAGMENT        (1u << 3)
#define RPBP_FL_LAST            (1u << 4)
#define RPBP_FL_CONTINUATION    (1u << 5)

#define RPBP_FL_RESERVED_MASK   ((uint8_t)0xC0)    /* bits 6,7 must be zero */

/*
 * FRAGMENT=1 + LAST=1 is illegal. The implementation SHOULD treat it
 * as a protocol error and respond with RPBP_EPROTO. Use rpbp_flags_valid()
 * to check.
 */
static inline bool rpbp_flags_valid(uint8_t flags)
{
    if ((flags & RPBP_FL_RESERVED_MASK) != 0)
        return false;
    if ((flags & RPBP_FL_FRAGMENT) && (flags & RPBP_FL_LAST))
        return false;
    return true;
}

/* ───── Channels ─────────────────────────────────────────────────────── */

#define RPBP_CHAN_CONTROL       ((uint16_t)0u)
#define RPBP_CHAN_EVENTS        ((uint16_t)1u)
#define RPBP_CHAN_DYN_MIN       ((uint16_t)16u)
#define RPBP_CHAN_DYN_MAX       ((uint16_t)239u)
#define RPBP_CHAN_VENDOR_MIN    ((uint16_t)240u)
#define RPBP_CHAN_VENDOR_MAX    ((uint16_t)254u)
#define RPBP_CHAN_BROADCAST     ((uint16_t)0xFFFFu)

/* ───── Subsystem IDs (CMD_REQUEST / CMD_RESPONSE payload byte 0) ───── */

enum rpbp_subsys {
    RPBP_SUBSYS_SYS    = 0x00,
    RPBP_SUBSYS_I2C    = 0x01,
    RPBP_SUBSYS_SPI    = 0x02,
    RPBP_SUBSYS_GPIO   = 0x03,
    RPBP_SUBSYS_PWM    = 0x04,
    RPBP_SUBSYS_ADC    = 0x05,
    RPBP_SUBSYS_UART   = 0x06,
    /* 0x07..0x7F reserved */
    /* 0x80..0xFF vendor */
};

/* ───── SYS opcodes (subsys = 0) ─────────────────────────────────────── */

enum rpbp_sys_op {
    RPBP_SYS_GET_CAPABILITIES = 0x00,
    RPBP_SYS_ECHO             = 0x01,
    RPBP_SYS_REBOOT_BOOTSEL   = 0x02,
    RPBP_SYS_UPTIME           = 0x03,
    RPBP_SYS_GET_VBUS_MV      = 0x04,
    RPBP_SYS_SET_LED          = 0x05,
    RPBP_SYS_SELFTEST         = 0x06,
    RPBP_SYS_GET_IDENTITY     = 0x07,
    RPBP_SYS_RESET            = 0x08,
};

/* ───── I²C opcodes (subsys = 1) ─────────────────────────────────────── */

enum rpbp_i2c_op {
    RPBP_I2C_PROBE    = 0x00,
    RPBP_I2C_XFER     = 0x01,
    RPBP_I2C_SCAN     = 0x02,
    RPBP_I2C_SET_FREQ = 0x03,
    RPBP_I2C_GET_FREQ = 0x04,
};

/*
 * I²C XFER flag bits. Bit 0 `NO_STOP` is the only runtime-selectable
 * behaviour knob in v1; bits 1..7 are reserved and MUST be zero.
 * The write-then-repeated-START-then-read pattern is chosen
 * automatically when both `tx_len` and `rx_len` are non-zero.
 */
#define RPBP_I2C_FLAG_NO_STOP     (1u << 0)

/* ───── UART opcodes (subsys = 6) ────────────────────────────────────── */

enum rpbp_uart_op {
    RPBP_UART_SET_PHY         = 0x00,
    RPBP_UART_GET_PHY         = 0x01,
};

/*
 * Phy selection applies only to UART instances that are backed by
 * the hardware phy-bank (rev-B: uart_idx = 0 → UART0 HW). PIO UART
 * instances (uart_idx = 1 → PIO-UART #2) are TTL-only and reject
 * SET_PHY with ENOTSUP.
 *
 * Integer values are stable wire ABI — they mirror
 * enum uart_phy_mode in firmware/src/periph/uart_phy.h.
 */
enum rpbp_uart_phy {
    RPBP_UART_PHY_OFF   = 0,
    RPBP_UART_PHY_TTL   = 1,
    RPBP_UART_PHY_RS232 = 2,
    RPBP_UART_PHY_RS485 = 3,
};

/* ───── Status codes ─────────────────────────────────────────────────── */

enum rpbp_status {
    RPBP_OK         = 0,
    RPBP_EAGAIN     = 1,
    RPBP_EINVAL     = 2,
    RPBP_EPERM      = 3,
    RPBP_ENOENT     = 4,
    RPBP_EIO        = 5,
    RPBP_ETIMEDOUT  = 6,
    RPBP_EMSGSIZE   = 7,
    RPBP_ECRC       = 8,
    RPBP_EPROTO     = 9,
    RPBP_ENOTSUP    = 10,
    RPBP_EBUSY      = 11,
    RPBP_ECANCELED  = 12,
    RPBP_ENOSPC     = 13,
    RPBP_EOVERFLOW  = 14,
};

/* ───── In-memory header view ────────────────────────────────────────── */

struct rpbp_header {
    uint8_t  magic;
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  flags;
    uint16_t channel;
    uint16_t seq;
    uint32_t payload_len;
    uint32_t timestamp_us;
};

#ifdef __cplusplus
}
#endif
#endif /* RPBP_H */
