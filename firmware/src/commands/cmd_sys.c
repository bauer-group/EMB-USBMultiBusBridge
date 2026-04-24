/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * SYS subsystem handlers — implementation of the opcode table in
 * docs/protocol/subsystems/sys.md.
 */

#include "cmd_sys.h"

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_framing.h"
#include "rpbp/rpbp_dispatch.h"
#include "rpbp/rpbp_cbor.h"
#include "capabilities/capabilities.h"
#include "version.h"

#include "pico/time.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"

#include <string.h>

/* ───── LED override state (consumed by main loop) ───────────────────── */

static struct {
    bool     active;
    uint8_t  r, g, b;
    uint8_t  mode;           /* 0=off, 1=solid, 2=blink, 3=pulse, 4=fast */
    uint8_t  brightness_pct; /* 0..100 */
} g_led_override;

bool cmd_sys_get_led_override(struct cmd_sys_led_override *out)
{
    if (!g_led_override.active) return false;
    if (out != NULL) {
        out->r              = g_led_override.r;
        out->g              = g_led_override.g;
        out->b              = g_led_override.b;
        out->mode           = g_led_override.mode;
        out->brightness_pct = g_led_override.brightness_pct;
    }
    return true;
}

int cmd_sys_set_led(uint8_t r, uint8_t g, uint8_t b,
                    uint8_t mode, uint8_t brightness_pct)
{
    if (mode > 4) {
        return -1;
    }
    if (brightness_pct > 100) brightness_pct = 100;

    if (mode == 0) {
        /* mode 0 releases the override so the firmware state machine
         * resumes control (heartbeat green). */
        g_led_override.active = false;
        return 0;
    }
    g_led_override.active         = true;
    g_led_override.r              = r;
    g_led_override.g              = g;
    g_led_override.b              = b;
    g_led_override.mode           = mode;
    g_led_override.brightness_pct = brightness_pct;
    return 0;
}

/* ───── HELLO ────────────────────────────────────────────────────────── */

int cmd_sys_reply_hello(const struct rpbp_header *req)
{
    /*
     * HELLO reply is a CBOR map carrying identity and protocol version.
     * It reuses the rpbridge_identity_emit() emitter for compactness;
     * nonce mirroring is not implemented in the scaffold (host-supplied
     * nonce from the request is not parsed — CBOR reader would add
     * weight and the nonce is best verified host-side via attestation
     * once that lands).
     */
    uint8_t buf[256];
    size_t  used = rpbridge_identity_emit(buf, sizeof(buf));
    if (used == 0)
        return -1;

    struct rpbp_header hdr = {
        .msg_type     = RPBP_MT_HELLO,
        .flags        = RPBP_FL_CBOR,
        .channel      = req->channel,
        .seq          = req->seq,
        .timestamp_us = (uint32_t)time_us_64(),
    };

    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  n = rpbp_frame_build(frame, &hdr, buf, used);
    if (n == 0) return -1;

    /* Piggy-back on the dispatcher's registered TX function. */
    struct rpbp_session *s = rpbp_session();
    return (s && s->tx) ? s->tx(frame, n) : -1;
}

/* ───── GET_CAPABILITIES ─────────────────────────────────────────────── */

static enum rpbp_status op_get_capabilities(const struct rpbp_header *req,
                                            bool cbor)
{
    (void)cbor;  /* capability response is CBOR-only regardless */

    uint8_t caps[2048];
    size_t  caps_len = rpbridge_capabilities_emit(caps, sizeof(caps));
    if (caps_len == 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_CAPABILITIES,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    /*
     * Wrap the capability map in the canonical CMD_RESPONSE CBOR shape:
     *   { "s": 0, "o": 0, "st": 0, "r": <capability-map> }
     *
     * We build the outer wrapper in a second buffer and inline the
     * caps bytes as pre-encoded CBOR.
     */
    uint8_t out[2048 + 32];
    struct cbor_writer w;
    cbor_writer_init(&w, out, sizeof(out));

    cbor_writer_map(&w, 4);
    cbor_writer_kv_uint(&w, "s",  RPBP_SUBSYS_SYS);
    cbor_writer_kv_uint(&w, "o",  RPBP_SYS_GET_CAPABILITIES);
    cbor_writer_kv_uint(&w, "st", RPBP_OK);
    cbor_writer_text_cstr(&w, "r");

    /* Inline: copy the pre-built CBOR capability map as raw bytes. */
    if (w.pos + caps_len > w.cap) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_CAPABILITIES,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    memcpy(&w.buf[w.pos], caps, caps_len);
    w.pos += caps_len;

    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_CAPABILITIES,
                   RPBP_OK, w.buf, w.pos, true);
    return RPBP_OK;
}

/* ───── ECHO ─────────────────────────────────────────────────────────── */

static enum rpbp_status op_echo(const struct rpbp_header *req,
                                const uint8_t *args, size_t args_len)
{
    /* Echo back the exact byte sequence; status=OK. */
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_ECHO, RPBP_OK,
                   args, args_len, false);
    return RPBP_OK;
}

/* ───── REBOOT_BOOTSEL ───────────────────────────────────────────────── */

static enum rpbp_status op_reboot_bootsel(const struct rpbp_header *req)
{
    /* Reply FIRST so the host sees the ACK before the USB link drops. */
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_REBOOT_BOOTSEL, RPBP_OK,
                   NULL, 0, false);

    /* Give TinyUSB a few ms to push the frame onto the wire. */
    sleep_ms(50);

    /* Arg 0: activity-LED GPIO mask (0 = firmware did it); arg 1: 0
     * = default behaviour (both MSC and PICOBOOT on). */
    reset_usb_boot(0, 0);
    /* unreachable */
    return RPBP_OK;
}

/* ───── UPTIME ───────────────────────────────────────────────────────── */

static enum rpbp_status op_uptime(const struct rpbp_header *req)
{
    uint64_t us = time_us_64();
    uint8_t  buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)((us >> (8 * i)) & 0xFFu);
    }
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_UPTIME, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── GET_VBUS_MV ──────────────────────────────────────────────────── */

static enum rpbp_status op_get_vbus_mv(const struct rpbp_header *req)
{
    /*
     * Scaffold: return a nominal 5000 mV. The real implementation
     * reads GP46 through a 100k/49.9k divider and an ADC, which lands
     * with the ADC subsystem handler.
     */
    uint16_t mv = 5000;
    uint8_t  buf[2] = {
        (uint8_t)(mv & 0xFFu),
        (uint8_t)((mv >> 8) & 0xFFu),
    };
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_VBUS_MV, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── SET_LED ──────────────────────────────────────────────────────── */

static enum rpbp_status op_set_led(const struct rpbp_header *req,
                                   const uint8_t *args, size_t args_len)
{
    if (args_len != 5) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_SET_LED,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    int rc = cmd_sys_set_led(args[0], args[1], args[2], args[3], args[4]);
    uint8_t status = (rc == 0) ? RPBP_OK : RPBP_EINVAL;
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_SET_LED, status,
                   NULL, 0, false);
    return RPBP_OK;
}

/* ───── SELFTEST (scaffold stub) ─────────────────────────────────────── */

static enum rpbp_status op_selftest(const struct rpbp_header *req,
                                    const uint8_t *args, size_t args_len)
{
    (void)args;
    if (args_len != 4) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_SELFTEST,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    /*
     * Scaffold: no tests registered yet, so pass_mask = 0 and no
     * failure blobs. Real peripheral loopbacks land with their
     * respective subsystems.
     */
    uint8_t buf[5] = { 0, 0, 0, 0, /* pass_mask LE */ 0 /* fails */ };
    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_SELFTEST, RPBP_OK,
                   buf, sizeof(buf), false);
    return RPBP_OK;
}

/* ───── GET_IDENTITY ─────────────────────────────────────────────────── */

static enum rpbp_status op_get_identity(const struct rpbp_header *req)
{
    uint8_t id[256];
    size_t  n = rpbridge_identity_emit(id, sizeof(id));
    if (n == 0) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_IDENTITY,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    /* Wrap in { "s":0, "o":7, "st":0, "r": <identity-map> } */
    uint8_t out[384];
    struct cbor_writer w;
    cbor_writer_init(&w, out, sizeof(out));
    cbor_writer_map(&w, 4);
    cbor_writer_kv_uint(&w, "s",  RPBP_SUBSYS_SYS);
    cbor_writer_kv_uint(&w, "o",  RPBP_SYS_GET_IDENTITY);
    cbor_writer_kv_uint(&w, "st", RPBP_OK);
    cbor_writer_text_cstr(&w, "r");
    if (w.pos + n > w.cap) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_IDENTITY,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    memcpy(&w.buf[w.pos], id, n);
    w.pos += n;

    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_GET_IDENTITY, RPBP_OK,
                   w.buf, w.pos, true);
    return RPBP_OK;
}

/* ───── RESET ────────────────────────────────────────────────────────── */

static enum rpbp_status op_reset(const struct rpbp_header *req,
                                 const uint8_t *args, size_t args_len)
{
    if (args_len != 1) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_RESET,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }
    uint8_t delay_ms = args[0];
    if (delay_ms > 200) delay_ms = 200;

    rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, RPBP_SYS_RESET, RPBP_OK,
                   NULL, 0, false);

    sleep_ms(delay_ms);
    watchdog_reboot(0, 0, 0);
    /* unreachable */
    return RPBP_OK;
}

/* ───── Dispatch ─────────────────────────────────────────────────────── */

enum rpbp_status cmd_sys_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor)
{
    switch (opcode) {
    case RPBP_SYS_GET_CAPABILITIES: return op_get_capabilities(req, cbor);
    case RPBP_SYS_ECHO:             return op_echo(req, args, args_len);
    case RPBP_SYS_REBOOT_BOOTSEL:   return op_reboot_bootsel(req);
    case RPBP_SYS_UPTIME:           return op_uptime(req);
    case RPBP_SYS_GET_VBUS_MV:      return op_get_vbus_mv(req);
    case RPBP_SYS_SET_LED:          return op_set_led(req, args, args_len);
    case RPBP_SYS_SELFTEST:         return op_selftest(req, args, args_len);
    case RPBP_SYS_GET_IDENTITY:     return op_get_identity(req);
    case RPBP_SYS_RESET:            return op_reset(req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_SYS, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
