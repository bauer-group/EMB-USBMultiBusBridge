/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * rpbp_stream — dynamic channel registry + STREAM_DATA emission.
 */

#include "rpbp_stream.h"

#include "rpbp.h"
#include "rpbp_framing.h"
#include "rpbp_dispatch.h"

#include "pico/time.h"

#include <string.h>

struct rpbp_stream_slot {
    bool     open;
    uint16_t channel;
    uint16_t next_seq;
    uint32_t credit;         /* bytes the host has permitted */
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t drop_credit;
    uint32_t drop_tx_fail;
};

static struct rpbp_stream_slot g_slots[RPBP_STREAM_MAX];

/* ───── Helpers ──────────────────────────────────────────────────────── */

static struct rpbp_stream_slot *find_slot(uint16_t channel)
{
    for (size_t i = 0; i < RPBP_STREAM_MAX; ++i) {
        if (g_slots[i].open && g_slots[i].channel == channel) return &g_slots[i];
    }
    return NULL;
}

static struct rpbp_stream_slot *find_free(void)
{
    for (size_t i = 0; i < RPBP_STREAM_MAX; ++i) {
        if (!g_slots[i].open) return &g_slots[i];
    }
    return NULL;
}

static bool channel_in_dyn_range(uint16_t channel)
{
    return channel >= RPBP_CHAN_DYN_MIN && channel <= RPBP_CHAN_DYN_MAX;
}

static uint32_t now_us32(void)
{
    return (uint32_t)time_us_64();
}

/* ───── Public API ───────────────────────────────────────────────────── */

void rpbp_stream_init(void)
{
    memset(g_slots, 0, sizeof(g_slots));
}

bool rpbp_stream_is_open(uint16_t channel)
{
    return find_slot(channel) != NULL;
}

int rpbp_stream_open(uint16_t channel)
{
    if (!channel_in_dyn_range(channel)) return -RPBP_EINVAL;
    if (find_slot(channel) != NULL)      return -RPBP_EBUSY;

    struct rpbp_stream_slot *s = find_free();
    if (s == NULL) return -RPBP_EBUSY;

    s->open         = true;
    s->channel      = channel;
    s->next_seq     = 0u;
    s->credit       = RPBP_STREAM_INITIAL_CREDIT;
    s->tx_frames    = 0u;
    s->tx_bytes     = 0u;
    s->drop_credit  = 0u;
    s->drop_tx_fail = 0u;
    return 0;
}

int rpbp_stream_close(uint16_t channel)
{
    struct rpbp_stream_slot *s = find_slot(channel);
    if (s == NULL) return -RPBP_ENOENT;
    s->open = false;      /* stats kept readable until the slot is reused */
    return 0;
}

void rpbp_stream_on_credit(uint16_t channel, uint32_t delta_bytes)
{
    struct rpbp_stream_slot *s = find_slot(channel);
    if (s == NULL) return;

    /* Saturate at u32 max — the spec caps credit at this width. */
    uint64_t next = (uint64_t)s->credit + (uint64_t)delta_bytes;
    s->credit = (next > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)next;
}

int rpbp_stream_send_frame(uint16_t channel,
                           const uint8_t *payload, size_t payload_len)
{
    struct rpbp_stream_slot *s = find_slot(channel);
    if (s == NULL)                              return -RPBP_ENOENT;
    if (payload_len > RPBP_MAX_PAYLOAD_LEN)     return -RPBP_EMSGSIZE;

    /* Credit accounting: charge the full payload length. If the host
     * hasn't advertised enough credit, drop + count. */
    if ((uint32_t)payload_len > s->credit) {
        s->drop_credit++;
        return -RPBP_EAGAIN;
    }

    struct rpbp_session *sess = rpbp_session();
    if (sess == NULL || sess->tx == NULL) return -RPBP_EIO;

    struct rpbp_header hdr = {
        .msg_type     = RPBP_MT_STREAM_DATA,
        .flags        = 0,
        .channel      = channel,
        .seq          = s->next_seq,
        .timestamp_us = now_us32(),
    };

    uint8_t frame[RPBP_MAX_FRAME_LEN];
    size_t  frame_len = rpbp_frame_build(frame, &hdr, payload, payload_len);
    if (frame_len == 0u) {
        s->drop_tx_fail++;
        return -RPBP_EIO;
    }

    int rc = sess->tx(frame, frame_len);
    if (rc != 0) {
        s->drop_tx_fail++;
        return -RPBP_EIO;
    }

    s->credit    -= (uint32_t)payload_len;
    s->tx_frames += 1u;
    s->tx_bytes  += (uint32_t)payload_len;
    s->next_seq  += 1u;
    return 0;
}

size_t rpbp_stream_snapshot(struct rpbp_stream_stats *out, size_t cap)
{
    size_t n = (cap < RPBP_STREAM_MAX) ? cap : RPBP_STREAM_MAX;
    for (size_t i = 0; i < n; ++i) {
        out[i].channel      = g_slots[i].channel;
        out[i].open         = g_slots[i].open;
        out[i].credit       = g_slots[i].credit;
        out[i].tx_frames    = g_slots[i].tx_frames;
        out[i].tx_bytes     = g_slots[i].tx_bytes;
        out[i].drop_credit  = g_slots[i].drop_credit;
        out[i].drop_tx_fail = g_slots[i].drop_tx_fail;
        out[i].next_seq     = g_slots[i].next_seq;
    }
    return RPBP_STREAM_MAX;
}
