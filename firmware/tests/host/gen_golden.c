/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * gen_golden — emit the canonical RPBP wire vectors to the on-disk
 * `tests/golden/*.hex` corpus.
 *
 * Usage: gen_golden <output-dir>
 *
 * Not a test — a utility. CMake wires it as the `gen-golden` target.
 * Runs at CI time; the resulting hex files are checked in and become
 * the byte-frozen contract both the firmware encoder and the Rust
 * driver decoder must honour.
 *
 * The file format is documented in firmware/tests/golden/README.md.
 */

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_framing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct golden_spec {
    const char    *filename;      /* file name under output dir       */
    const char    *description;   /* first-line comment inside file   */
    uint8_t        msg_type;
    uint16_t       channel;
    uint16_t       seq;
    uint32_t       timestamp_us;
    const uint8_t *payload;
    size_t         payload_len;
};

/* Representative corpus — keep small + deterministic. Payload
 * bytes use human-readable / fixed values so a reader can eyeball
 * the hex file against the spec. */

static const uint8_t ECHO_PAYLOAD[]    = { RPBP_SUBSYS_SYS, RPBP_SYS_ECHO,
                                           'H', 'i', '!' };
static const uint8_t I2C_PROBE_ARGS[]  = { RPBP_SUBSYS_I2C, RPBP_I2C_PROBE,
                                           0x00 /* bus */, 0x50 /* addr */ };
static const uint8_t GPIO_EVENT_PL[]   = { RPBP_SUBSYS_GPIO, 0x00, /* edge */
                                           30, 1,
                                           0x78, 0x56, 0x34, 0x12 };
static const uint8_t STREAM_DATA_PL[]  = { 0x78, 0x56, 0x34, 0x12,  /* ts us */
                                           0x01, 1, 0xAB, 0xCD };   /* mask=ch0, 1 sample */

static const struct golden_spec CORPUS[] = {
    {
        .filename    = "hello_empty.hex",
        .description = "HELLO empty-payload, control channel",
        .msg_type    = RPBP_MT_HELLO,
        .channel     = RPBP_CHAN_CONTROL,
        .seq         = 0,
        .timestamp_us= 0,
        .payload     = NULL, .payload_len = 0,
    },
    {
        .filename    = "ping_ctrl_seq42.hex",
        .description = "PING on control channel, seq=0x2A",
        .msg_type    = RPBP_MT_PING,
        .channel     = RPBP_CHAN_CONTROL,
        .seq         = 0x002A,
        .timestamp_us= 0x11223344u,
        .payload     = NULL, .payload_len = 0,
    },
    {
        .filename    = "cmd_sys_echo_hi.hex",
        .description = "CMD_REQUEST SYS ECHO payload='Hi!'",
        .msg_type    = RPBP_MT_CMD_REQUEST,
        .channel     = RPBP_CHAN_CONTROL,
        .seq         = 1,
        .timestamp_us= 0,
        .payload     = ECHO_PAYLOAD, .payload_len = sizeof(ECHO_PAYLOAD),
    },
    {
        .filename    = "cmd_i2c_probe.hex",
        .description = "CMD_REQUEST I2C PROBE bus=0 addr=0x50",
        .msg_type    = RPBP_MT_CMD_REQUEST,
        .channel     = RPBP_CHAN_CONTROL,
        .seq         = 2,
        .timestamp_us= 0,
        .payload     = I2C_PROBE_ARGS, .payload_len = sizeof(I2C_PROBE_ARGS),
    },
    {
        .filename    = "event_gpio_edge.hex",
        .description = "EVENT on channel 1: GPIO edge pin=30 level=1 ts=0x12345678",
        .msg_type    = RPBP_MT_EVENT,
        .channel     = RPBP_CHAN_EVENTS,
        .seq         = 0,
        .timestamp_us= 0,
        .payload     = GPIO_EVENT_PL, .payload_len = sizeof(GPIO_EVENT_PL),
    },
    {
        .filename    = "stream_data_adc.hex",
        .description = "STREAM_DATA on channel 16 (ADC stream, 1 sample)",
        .msg_type    = RPBP_MT_STREAM_DATA,
        .channel     = RPBP_CHAN_DYN_MIN,
        .seq         = 0,
        .timestamp_us= 0x12345678u,
        .payload     = STREAM_DATA_PL, .payload_len = sizeof(STREAM_DATA_PL),
    },
    {
        .filename    = "reset_events.hex",
        .description = "RESET_CHANNEL on events channel",
        .msg_type    = RPBP_MT_RESET_CHANNEL,
        .channel     = RPBP_CHAN_EVENTS,
        .seq         = 5,
        .timestamp_us= 0,
        .payload     = NULL, .payload_len = 0,
    },
};

#define CORPUS_COUNT (sizeof(CORPUS) / sizeof(CORPUS[0]))

static int write_hex_file(const char *path, const struct golden_spec *s,
                          const uint8_t *frame, size_t frame_len)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "gen_golden: cannot open %s for writing\n", path);
        return -1;
    }

    fprintf(f, "# %s\n", s->description);
    fprintf(f, "# msg_type=0x%02X channel=%u seq=%u payload_len=%zu "
               "ts_us=0x%08X\n",
            (unsigned)s->msg_type, s->channel, s->seq,
            s->payload_len, s->timestamp_us);

    for (size_t i = 0; i < frame_len; ++i) {
        fprintf(f, "%02x", frame[i]);
        if ((i + 1) % 16u == 0u || i + 1u == frame_len) fputc('\n', f);
        else                                             fputc(' ', f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    const char *out_dir = argv[1];

    for (size_t i = 0; i < CORPUS_COUNT; ++i) {
        const struct golden_spec *s = &CORPUS[i];
        struct rpbp_header h = {
            .msg_type     = s->msg_type,
            .flags        = 0,
            .channel      = s->channel,
            .seq          = s->seq,
            .timestamp_us = s->timestamp_us,
        };

        uint8_t frame[RPBP_MAX_FRAME_LEN];
        size_t  frame_len = rpbp_frame_build(frame, &h,
                                             s->payload, s->payload_len);
        if (frame_len == 0u) {
            fprintf(stderr, "gen_golden: failed to build %s\n", s->filename);
            return 3;
        }

        char path[512];
        int  n = snprintf(path, sizeof(path), "%s/%s", out_dir, s->filename);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "gen_golden: path too long\n");
            return 4;
        }
        if (write_hex_file(path, s, frame, frame_len) != 0) return 5;
        fprintf(stdout, "wrote %s (%zu bytes)\n", path, frame_len);
    }
    return 0;
}
