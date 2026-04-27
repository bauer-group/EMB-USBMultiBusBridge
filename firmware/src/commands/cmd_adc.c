/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * ADC subsystem dispatcher. Wire format in
 * docs/protocol/subsystems/adc.md.
 */

#include "cmd_adc.h"

#include "peripherals/adc.h"
#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"
#include "rpbp/rpbp_stream.h"
#include "io_control/io_control.h"

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

/* ───── 0x00 — READ ──────────────────────────────────────────────────── */

static enum rpbp_status op_read(const struct rpbp_header *req,
                                const uint8_t *args, size_t args_len)
{
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_READ,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t channel = args[0];
    int raw = adc_hw_read_raw(channel);
    if (raw < 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_READ,
                       rc_to_status(raw), NULL, 0, false);
        return RPBP_OK;
    }
    int mv = (raw * (int)adc_hw_vref_mv()) / 4095;

    uint8_t buf[4];
    wr_le16(&buf[0], (uint16_t)raw);
    wr_le16(&buf[2], (uint16_t)mv);
    rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_READ, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x01 — READ_MANY ─────────────────────────────────────────────── */

static enum rpbp_status op_read_many(const struct rpbp_header *req,
                                     const uint8_t *args, size_t args_len)
{
    if (args_len != 2u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_READ_MANY,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint16_t mask = rd_le16(args);

    /*
     * Reply: count u8 | channel0 u8 | raw0 u16 | mv0 u16 | … |
     *        channelN-1 u8 | rawN-1 u16 | mvN-1 u16.
     *
     * Per-channel tuple = 5 bytes; up to 5 channels (0..4) = 26 bytes
     * total. Comfortably fits the stack.
     */
    uint8_t buf[1u + 5u * 5u];
    size_t off = 1u;
    uint8_t count = 0u;

    for (uint8_t ch = 0u; ch < 5u; ++ch) {
        if ((mask & (1u << ch)) == 0u) continue;
        int raw = adc_hw_read_raw(ch);
        if (raw < 0) {
            rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_READ_MANY,
                           rc_to_status(raw), NULL, 0, false);
            return RPBP_OK;
        }
        int mv = (raw * (int)adc_hw_vref_mv()) / 4095;

        buf[off + 0] = ch;
        wr_le16(&buf[off + 1], (uint16_t)raw);
        wr_le16(&buf[off + 3], (uint16_t)mv);
        off += 5u;
        ++count;
    }
    buf[0] = count;
    rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_READ_MANY, RPBP_OK,
                   buf, off, false);
    return RPBP_OK;
}

/* ───── 0x02 — GET_REF ───────────────────────────────────────────────── */

static enum rpbp_status op_get_ref(const struct rpbp_header *req,
                                   const uint8_t *args, size_t args_len)
{
    (void)args;
    if (args_len != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_GET_REF,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t buf[4];
    wr_le32(buf, adc_hw_vref_mv());
    rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_GET_REF, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x03 — TEMP_READ ─────────────────────────────────────────────── */

static enum rpbp_status op_temp_read(const struct rpbp_header *req,
                                     const uint8_t *args, size_t args_len)
{
    (void)args;
    if (args_len != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_TEMP_READ,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    int16_t cdeg;
    int rc = adc_hw_read_temp_cdegC(&cdeg);
    if (rc < 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_TEMP_READ,
                       rc_to_status(rc), NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t buf[2];
    wr_le16(buf, (uint16_t)cdeg);
    rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_TEMP_READ, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── 0x04 — STREAM_START ──────────────────────────────────────────── */

/*
 * Wire format: [channel LE u16][channel_mask u8][reserved u8][rate_hz LE u32]
 *   channel       — dynamic channel (16..239) host proposes for the stream
 *   channel_mask  — ADC channels to sample, bits 0..4 (ch 0..3 + temp)
 *   rate_hz       — sample rate per channel-set (sets/s).
 *                   Clamped to 1..10000 — USB FS bandwidth ceiling.
 */
static enum rpbp_status op_stream_start(const struct rpbp_header *req,
                                        const uint8_t *args, size_t args_len)
{
    if (args_len != 8u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_STREAM_START,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint16_t channel      = rd_le16(&args[0]);
    uint8_t  channel_mask = args[2];
    /* args[3] reserved */
    uint32_t rate_hz      = (uint32_t)args[4]
                          | ((uint32_t)args[5] << 8)
                          | ((uint32_t)args[6] << 16)
                          | ((uint32_t)args[7] << 24);

    if (channel_mask == 0u || (channel_mask & 0xE0u) != 0u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_STREAM_START,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }
    if (rate_hz == 0u)     rate_hz = 1u;
    if (rate_hz > 10000u)  rate_hz = 10000u;

    int rc = rpbp_stream_open(channel);
    if (rc < 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_STREAM_START,
                       rc_to_status(rc), NULL, 0, false);
        return RPBP_OK;
    }

    /* Start the Core-1 sampler + point the drain at this channel. */
    uint32_t period_us = 1000000u / rate_hz;
    io_control_adc_stream_channel(channel);
    io_control_adc_configure(channel_mask, period_us);

    uint8_t reply[2];
    wr_le16(reply, channel);
    rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_STREAM_START, RPBP_OK,
                   reply, sizeof(reply), false);
    return RPBP_OK;
}

/* ───── 0x05 — STREAM_STOP ───────────────────────────────────────────── */

static enum rpbp_status op_stream_stop(const struct rpbp_header *req,
                                       const uint8_t *args, size_t args_len)
{
    if (args_len != 2u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_STREAM_STOP,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint16_t channel = rd_le16(&args[0]);

    /* Stop the producer first — any in-flight samples in the IPC ring
     * are drained harmlessly by io_control_poll once the channel is
     * closed. Order matters: closing the channel before stopping the
     * producer would widen the drop window. */
    io_control_adc_configure(0u, 0u);
    io_control_adc_stream_channel(0xFFFFu);

    int rc = rpbp_stream_close(channel);
    rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, RPBP_ADC_STREAM_STOP,
                   rc_to_status(rc), NULL, 0, false);
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_adc_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor)
{
    (void)cbor;

    switch (opcode) {
    case RPBP_ADC_READ:         return op_read       (req, args, args_len);
    case RPBP_ADC_READ_MANY:    return op_read_many  (req, args, args_len);
    case RPBP_ADC_GET_REF:      return op_get_ref    (req, args, args_len);
    case RPBP_ADC_TEMP_READ:    return op_temp_read  (req, args, args_len);
    case RPBP_ADC_STREAM_START: return op_stream_start(req, args, args_len);
    case RPBP_ADC_STREAM_STOP:  return op_stream_stop (req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_ADC, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
