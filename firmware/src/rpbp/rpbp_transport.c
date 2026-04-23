/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * TinyUSB vendor-class glue for RPBP.
 *
 * ─── RX state machine ───────────────────────────────────────────────
 *
 * States:
 *   HUNT       : looking for the MAGIC byte. Dropped bytes are counted
 *                as resyncs.
 *   HEADER     : have MAGIC at offset 0, accumulating up to 16 bytes.
 *   BODY       : header parsed and valid, accumulating payload + CRC.
 *
 * Transitions:
 *   HUNT   : on seeing 0x52 -> HEADER, pos = 1.
 *   HEADER : on 16 bytes -> parse; if bad -> HUNT (bytes count as
 *            resync), emit ERROR. If good -> BODY, need
 *            payload_len + CRC_LEN additional bytes.
 *   BODY   : on full frame -> validate CRC; if bad, emit ERROR, go
 *            HUNT; else dispatch, go HUNT (start fresh).
 *
 * The "bytes count as resync" paragraph is important. When we reject
 * a parsed header we don't know how many of the 16 bytes were noise
 * vs a valid second start. We play it safe: discard the first byte
 * and rescan, so a stuck 0x52 at position 0 doesn't loop forever.
 *
 * ─── TX ─────────────────────────────────────────────────────────────
 *
 * tud_vendor_n_write() takes up to a 4 KB FIFO (CFG_TUD_VENDOR_TX_BUFSIZE
 * in tusb_config.h). When the FIFO is full we spin briefly while
 * tud_task() drains it. If the interface is not mounted at all, we
 * fail fast rather than blocking.
 */

#include "rpbp_transport.h"
#include "rpbp.h"
#include "rpbp_framing.h"
#include "rpbp_dispatch.h"

#include "tusb.h"
#include "pico/time.h"

#include <string.h>

/*
 * Vendor interface index for the RPBP pipe (first VENDOR interface in
 * the descriptor set). The second VENDOR interface (index 1) is the
 * gs_usb CAN pipe, handled by the gs_usb glue, not here.
 */
#ifndef RPBP_VENDOR_ITF
#define RPBP_VENDOR_ITF  0
#endif

/* ───── RX buffer + state ────────────────────────────────────────────── */

enum rx_state {
    RX_HUNT = 0,
    RX_HEADER,
    RX_BODY,
};

static struct {
    enum rx_state       state;
    uint8_t             buf[RPBP_MAX_FRAME_LEN];
    size_t              pos;         /* bytes buffered so far */
    size_t              frame_len;   /* total expected once in BODY */
    struct rpbp_header  hdr;         /* parsed once we leave HEADER */
} g_rx;

/* ───── Stats ────────────────────────────────────────────────────────── */

static struct rpbp_transport_stats g_stats;

const struct rpbp_transport_stats *rpbp_transport_stats(void)
{
    return &g_stats;
}

/* ───── TX implementation ────────────────────────────────────────────── */

int rpbp_transport_send(const uint8_t *frame, size_t frame_len)
{
    if (!tud_vendor_n_mounted(RPBP_VENDOR_ITF)) {
        g_stats.tx_drops++;
        return -1;
    }

    const uint32_t deadline_ms = 200;
    const uint32_t start = to_ms_since_boot(get_absolute_time());

    size_t written = 0;
    while (written < frame_len) {
        uint32_t n = tud_vendor_n_write(RPBP_VENDOR_ITF,
                                        frame + written,
                                        (uint32_t)(frame_len - written));
        if (n == 0) {
            /* FIFO full — give TinyUSB a chance to drain. */
            tud_task();
            if (to_ms_since_boot(get_absolute_time()) - start > deadline_ms) {
                g_stats.tx_drops++;
                return -1;
            }
            continue;
        }
        written += n;
    }

    tud_vendor_n_flush(RPBP_VENDOR_ITF);
    g_stats.tx_frames++;
    return 0;
}

/* ───── RX helpers ───────────────────────────────────────────────────── */

static void rx_reset_to_hunt(void)
{
    g_rx.state     = RX_HUNT;
    g_rx.pos       = 0;
    g_rx.frame_len = 0;
}

/* Shift the buffer left by one byte and continue the search. Called
 * when the current byte at pos 0 turned out not to start a valid
 * frame (wrong header after 16 bytes). */
static void rx_resync_shift(void)
{
    if (g_rx.pos > 0) {
        memmove(g_rx.buf, g_rx.buf + 1, g_rx.pos - 1);
        g_rx.pos -= 1;
    }
    g_rx.state     = RX_HUNT;
    g_rx.frame_len = 0;
    g_stats.rx_resyncs++;

    /* Any remaining bytes already in the buffer might begin a new
     * frame. Scan forward. */
    while (g_rx.pos > 0 && g_rx.buf[0] != RPBP_MAGIC) {
        memmove(g_rx.buf, g_rx.buf + 1, g_rx.pos - 1);
        g_rx.pos -= 1;
        g_stats.rx_resyncs++;
    }
    if (g_rx.pos > 0) {
        g_rx.state = RX_HEADER;
    }
}

/* Process buffered bytes advancing the state machine. Returns the
 * number of complete frames dispatched. */
static unsigned rx_drive(void)
{
    unsigned dispatched = 0;

    for (;;) {
        switch (g_rx.state) {
        case RX_HUNT:
            return dispatched;

        case RX_HEADER:
            if (g_rx.pos < RPBP_HEADER_LEN) {
                return dispatched;
            }
            {
                enum rpbp_status s = rpbp_header_parse(g_rx.buf, &g_rx.hdr);
                if (s != RPBP_OK) {
                    /* Bad header — emit ERROR then resync. */
                    g_stats.rx_frames_bad++;
                    rpbp_reply_error(NULL, (uint8_t)s, "bad header");
                    rx_resync_shift();
                    continue;
                }
                g_rx.frame_len = (size_t)RPBP_HEADER_LEN
                               + g_rx.hdr.payload_len
                               + RPBP_CRC_LEN;
                g_rx.state = RX_BODY;
            }
            /* fallthrough */

        case RX_BODY:
            if (g_rx.pos < g_rx.frame_len) {
                return dispatched;
            }
            {
                struct rpbp_header hdr_check;
                enum rpbp_status   vs = rpbp_frame_validate(g_rx.buf,
                                                            g_rx.frame_len,
                                                            &hdr_check);
                if (vs != RPBP_OK) {
                    g_stats.rx_frames_bad++;
                    /* Emit an ERROR tied to the (channel,seq) we did
                     * manage to parse; the status byte tells the host
                     * what failed. */
                    rpbp_reply_error(&g_rx.hdr, (uint8_t)vs,
                                     (vs == RPBP_ECRC) ? "crc" : "frame");
                    rx_reset_to_hunt();
                    /* any trailing bytes beyond this frame need to be
                     * preserved; but frame_len = pos here, so nothing
                     * trails. */
                    continue;
                }

                g_stats.rx_frames_ok++;
                const uint8_t *payload = g_rx.buf + RPBP_HEADER_LEN;
                rpbp_dispatch_frame(&hdr_check, payload, hdr_check.payload_len);

                /* Preserve any bytes that arrived past this frame. */
                size_t leftover = g_rx.pos - g_rx.frame_len;
                if (leftover > 0) {
                    memmove(g_rx.buf, g_rx.buf + g_rx.frame_len, leftover);
                }
                g_rx.pos       = leftover;
                g_rx.frame_len = 0;
                g_rx.state     = (leftover > 0 && g_rx.buf[0] == RPBP_MAGIC)
                                 ? RX_HEADER : RX_HUNT;
                if (g_rx.state == RX_HUNT && leftover > 0) {
                    /* Scan forward for MAGIC. */
                    while (g_rx.pos > 0 && g_rx.buf[0] != RPBP_MAGIC) {
                        memmove(g_rx.buf, g_rx.buf + 1, g_rx.pos - 1);
                        g_rx.pos -= 1;
                        g_stats.rx_resyncs++;
                    }
                    if (g_rx.pos > 0) g_rx.state = RX_HEADER;
                }
                dispatched++;
            }
            break;
        }
    }
}

/* ───── Ingest bytes from the vendor endpoint ────────────────────────── */

static void rx_ingest(const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        if (g_rx.state == RX_HUNT) {
            /* Skip non-MAGIC noise. */
            while (i < len && data[i] != RPBP_MAGIC) {
                i++;
                g_stats.rx_resyncs++;
            }
            if (i >= len) return;
            /* Found MAGIC. */
            g_rx.buf[0] = RPBP_MAGIC;
            g_rx.pos    = 1;
            g_rx.state  = RX_HEADER;
            i++;
        }

        /* In HEADER or BODY — append as much as we can without
         * exceeding the buffer. We always have room for one more
         * frame: g_rx.buf is sized for RPBP_MAX_FRAME_LEN. */
        size_t room = RPBP_MAX_FRAME_LEN - g_rx.pos;
        size_t take = (len - i) < room ? (len - i) : room;
        if (take == 0) {
            /* Ran out of room — that would be a malformed frame
             * claiming more than MAX_FRAME_LEN bytes. rpbp_header_parse
             * bounds it, so this is only defensive. */
            rx_reset_to_hunt();
            break;
        }
        memcpy(g_rx.buf + g_rx.pos, data + i, take);
        g_rx.pos += take;
        i        += take;
    }
}

/* ───── Public pump ──────────────────────────────────────────────────── */

unsigned rpbp_transport_poll(void)
{
    if (!tud_vendor_n_mounted(RPBP_VENDOR_ITF)) {
        return 0;
    }

    /* Drain at most CFG_TUD_VENDOR_RX_BUFSIZE bytes per call to keep
     * the service loop bounded. */
    uint8_t  scratch[256];
    unsigned dispatched = 0;

    while (tud_vendor_n_available(RPBP_VENDOR_ITF) > 0) {
        uint32_t n = tud_vendor_n_read(RPBP_VENDOR_ITF,
                                       scratch, sizeof(scratch));
        if (n == 0) break;

        rx_ingest(scratch, (size_t)n);
        dispatched += rx_drive();
    }

    return dispatched;
}

/* ───── Init ─────────────────────────────────────────────────────────── */

void rpbp_transport_init(void)
{
    memset(&g_rx,    0, sizeof(g_rx));
    memset(&g_stats, 0, sizeof(g_stats));
    rx_reset_to_hunt();
    rpbp_dispatch_init(rpbp_transport_send);
}
