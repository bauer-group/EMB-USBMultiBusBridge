/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * RPBP dispatcher + reply builders.
 *
 * All reply paths allocate their scratch buffer on the stack — frames
 * are at most RPBP_MAX_FRAME_LEN (4116 B), which fits comfortably in
 * the 4 KB task stack the pico-sdk grants main()/the USB task.
 */

#include "rpbp_dispatch.h"

#include "pico/time.h"

#include "cmd/cmd_sys.h"

#include <string.h>

/* ───── Session ──────────────────────────────────────────────────────── */

static struct rpbp_session g_session;

struct rpbp_session *rpbp_session(void)
{
    return &g_session;
}

void rpbp_dispatch_init(rpbp_tx_fn tx)
{
    g_session.tx             = tx;
    g_session.open           = false;
    g_session.next_ctrl_seq  = 1;  /* seq 0 reserved for HELLO reply */
    g_session.next_event_seq = 0;
}

void rpbp_dispatch_reset_session(void)
{
    g_session.open           = false;
    g_session.next_ctrl_seq  = 1;
    g_session.next_event_seq = 0;
}

/* ───── Reply building (internal helper) ─────────────────────────────── */

/*
 * Timestamp source for outgoing frames. The pico-sdk's time_us_64()
 * monotonic µs counter is truncated to u32. Wraps every ~71.6 min;
 * host uses PING/PONG offsets to stitch across the wrap.
 */
static uint32_t now_us32(void)
{
    return (uint32_t)time_us_64();
}

static int send_frame(struct rpbp_header *hdr,
                      const uint8_t *payload, size_t payload_len)
{
    if (g_session.tx == NULL)
        return -1;

    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  frame_len = rpbp_frame_build(frame, hdr, payload, payload_len);
    if (frame_len == 0)
        return -1;

    return g_session.tx(frame, frame_len);
}

/* ───── CMD_RESPONSE ─────────────────────────────────────────────────── */

int rpbp_reply_cmd(const struct rpbp_header *req,
                   uint8_t subsys, uint8_t opcode, uint8_t status,
                   const uint8_t *result, size_t result_len,
                   bool cbor_flag)
{
    struct rpbp_header hdr = {
        .msg_type     = RPBP_MT_CMD_RESPONSE,
        .flags        = cbor_flag ? RPBP_FL_CBOR : 0,
        .channel      = req->channel,
        .seq          = req->seq,
        .timestamp_us = now_us32(),
    };

    if (cbor_flag) {
        /* CBOR response carries its own structure; caller supplies the
         * full payload bytes. See rpbp-v1.md §10.2. */
        return send_frame(&hdr, result, result_len);
    }

    /* Binary response: [subsys:u8][opcode:u8][status:u8][result...] */
    uint8_t buf[RPBP_MAX_PAYLOAD_LEN];
    if (result_len + 3 > sizeof(buf))
        return -1;
    buf[0] = subsys;
    buf[1] = opcode;
    buf[2] = status;
    if (result_len > 0 && result != NULL) {
        memcpy(&buf[3], result, result_len);
    }
    return send_frame(&hdr, buf, 3 + result_len);
}

/* ───── ERROR ────────────────────────────────────────────────────────── */

int rpbp_reply_error(const struct rpbp_header *req,
                     uint8_t status, const char *reason)
{
    struct rpbp_header hdr = {
        .msg_type     = RPBP_MT_ERROR,
        .flags        = 0,
        .channel      = RPBP_CHAN_CONTROL,
        .seq          = g_session.next_ctrl_seq++,
        .timestamp_us = now_us32(),
    };

    uint8_t buf[RPBP_MAX_PAYLOAD_LEN];
    buf[0] = status;

    uint16_t orig_ch  = (req != NULL) ? req->channel : 0u;
    uint16_t orig_seq = (req != NULL) ? req->seq     : 0u;

    buf[1] = (uint8_t)(orig_ch & 0xFFu);
    buf[2] = (uint8_t)((orig_ch >> 8) & 0xFFu);
    buf[3] = (uint8_t)(orig_seq & 0xFFu);
    buf[4] = (uint8_t)((orig_seq >> 8) & 0xFFu);

    size_t reason_len = 0;
    if (reason != NULL) {
        reason_len = strlen(reason);
        if (reason_len > 255) reason_len = 255;
    }

    buf[5] = (uint8_t)(reason_len & 0xFFu);
    buf[6] = (uint8_t)((reason_len >> 8) & 0xFFu);

    if (reason_len > 0) {
        memcpy(&buf[7], reason, reason_len);
    }

    return send_frame(&hdr, buf, 7 + reason_len);
}

/* ───── PONG / TIME_SYNC ─────────────────────────────────────────────── */

int rpbp_reply_pong(const struct rpbp_header *req, uint32_t dev_rx_us)
{
    struct rpbp_header hdr = {
        .msg_type     = RPBP_MT_PONG,
        .flags        = 0,
        .channel      = req->channel,
        .seq          = req->seq,
        .timestamp_us = now_us32(),       /* = t3, device send */
    };

    /* Payload: [t2_dev_rx_us : LE u32]  (t1/t4 are host-local) */
    uint8_t buf[4];
    buf[0] = (uint8_t)(dev_rx_us & 0xFFu);
    buf[1] = (uint8_t)((dev_rx_us >> 8) & 0xFFu);
    buf[2] = (uint8_t)((dev_rx_us >> 16) & 0xFFu);
    buf[3] = (uint8_t)((dev_rx_us >> 24) & 0xFFu);
    return send_frame(&hdr, buf, sizeof(buf));
}

/* ───── EVENT ────────────────────────────────────────────────────────── */

int rpbp_send_event(uint8_t flags,
                    const uint8_t *payload, size_t payload_len)
{
    struct rpbp_header hdr = {
        .msg_type     = RPBP_MT_EVENT,
        .flags        = flags & ~RPBP_FL_RESERVED_MASK,
        .channel      = RPBP_CHAN_EVENTS,
        .seq          = g_session.next_event_seq++,
        .timestamp_us = now_us32(),
    };
    return send_frame(&hdr, payload, payload_len);
}

/* ───── Incoming-frame routing ───────────────────────────────────────── */

static enum rpbp_status handle_hello(const struct rpbp_header *hdr,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    /*
     * Host HELLO is informational — for bring-up we accept it as long
     * as the header is well-formed. The spec says the full check is
     * against the CBOR proto triple; we trust the header's version
     * byte in v1 and reply with our own HELLO.
     *
     * On a successful handshake, mark the session open so that later
     * requests are accepted. On mismatch, send ERROR { ENOTSUP }.
     */
    (void)payload;
    (void)payload_len;

    if (hdr->version != RPBP_VERSION) {
        return rpbp_reply_error(hdr, RPBP_ENOTSUP, "proto version") == 0
                   ? RPBP_OK : RPBP_EIO;
    }

    int rc = cmd_sys_reply_hello(hdr);
    if (rc == 0) {
        g_session.open = true;
        return RPBP_OK;
    }
    return RPBP_EIO;
}

static enum rpbp_status handle_ping(const struct rpbp_header *hdr,
                                    const uint8_t *payload,
                                    size_t payload_len)
{
    (void)payload;
    (void)payload_len;
    /* Capture t2 as soon as possible. The header already has the
     * host's t1 implicitly (host's own tracking); t3 is populated on
     * send. */
    uint32_t t2 = now_us32();
    return rpbp_reply_pong(hdr, t2) == 0 ? RPBP_OK : RPBP_EIO;
}

static enum rpbp_status handle_reset_channel(const struct rpbp_header *hdr,
                                             const uint8_t *payload,
                                             size_t payload_len)
{
    (void)payload;
    (void)payload_len;

    /*
     * For v1 scaffolding the only mutable channel state we hold is the
     * device-local seq counters. Reset them when the host resets the
     * control channel; otherwise the request is a no-op for now (no
     * dynamic channels are open in the scaffold).
     */
    if (hdr->channel == RPBP_CHAN_CONTROL) {
        g_session.next_ctrl_seq = 1;
    } else if (hdr->channel == RPBP_CHAN_EVENTS) {
        g_session.next_event_seq = 0;
    }

    /* Acknowledge with CMD_RESPONSE-style OK (subsys=0, opcode=0 is a
     * legal "ack" in the scaffold; the reply stays on the same channel
     * and seq as the request per RESET_CHANNEL semantics). */
    struct rpbp_header ack = {
        .msg_type     = RPBP_MT_CMD_RESPONSE,
        .flags        = 0,
        .channel      = hdr->channel,
        .seq          = hdr->seq,
        .timestamp_us = now_us32(),
    };
    uint8_t buf[3] = { 0, 0, RPBP_OK };
    return send_frame(&ack, buf, sizeof(buf)) == 0 ? RPBP_OK : RPBP_EIO;
}

static enum rpbp_status handle_cmd_request(const struct rpbp_header *hdr,
                                           const uint8_t *payload,
                                           size_t payload_len)
{
    if (payload_len < 2) {
        rpbp_reply_error(hdr, RPBP_EMSGSIZE, "cmd too short");
        return RPBP_OK;
    }

    uint8_t subsys = payload[0];
    uint8_t opcode = payload[1];
    const uint8_t *args = payload + 2;
    size_t args_len = payload_len - 2;
    bool cbor = (hdr->flags & RPBP_FL_CBOR) != 0;

    switch (subsys) {
    case RPBP_SUBSYS_SYS:
        return cmd_sys_dispatch(hdr, opcode, args, args_len, cbor);

    case RPBP_SUBSYS_I2C:
    case RPBP_SUBSYS_SPI:
    case RPBP_SUBSYS_GPIO:
    case RPBP_SUBSYS_PWM:
    case RPBP_SUBSYS_ADC:
    case RPBP_SUBSYS_UART:
        rpbp_reply_cmd(hdr, subsys, opcode, RPBP_ENOTSUP, NULL, 0, false);
        return RPBP_OK;

    default:
        rpbp_reply_cmd(hdr, subsys, opcode, RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }
}

enum rpbp_status rpbp_dispatch_frame(const struct rpbp_header *hdr,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    if (hdr == NULL) return RPBP_EINVAL;

    switch (hdr->msg_type) {
    case RPBP_MT_HELLO:
        return handle_hello(hdr, payload, payload_len);

    case RPBP_MT_PING:
    case RPBP_MT_TIME_SYNC:
        return handle_ping(hdr, payload, payload_len);

    case RPBP_MT_CMD_REQUEST:
        if (!g_session.open) {
            rpbp_reply_error(hdr, RPBP_EPERM, "session not open");
            return RPBP_OK;
        }
        return handle_cmd_request(hdr, payload, payload_len);

    case RPBP_MT_RESET_CHANNEL:
        return handle_reset_channel(hdr, payload, payload_len);

    case RPBP_MT_STREAM_CREDIT:
        /* Credit accounting for v1 scaffold: accept silently. Actual
         * flow control lives in the channel layer once streams are
         * wired up (not in this scaffold). */
        return RPBP_OK;

    /* Device-to-host only — host sending these is a protocol error. */
    case RPBP_MT_CAPABILITIES:
    case RPBP_MT_CMD_RESPONSE:
    case RPBP_MT_STREAM_DATA:
    case RPBP_MT_EVENT:
    case RPBP_MT_PONG:
    case RPBP_MT_ERROR:
        rpbp_reply_error(hdr, RPBP_EPROTO, "wrong direction");
        return RPBP_OK;

    default:
        rpbp_reply_error(hdr, RPBP_EPROTO, "unknown msg_type");
        return RPBP_OK;
    }
}
