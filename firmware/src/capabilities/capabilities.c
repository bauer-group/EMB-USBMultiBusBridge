/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Capability-map emitter.
 *
 * The schema lives in docs/protocol/capabilities.md. Any new field
 * added here must be reflected there — the capability map is the
 * device's self-description and the primary forward-compatibility
 * mechanism, so drift is a real risk.
 *
 * Philosophy:
 *   - Small, hand-written, definite-length CBOR. No external dep.
 *   - Fields are emitted in a stable order; hosts MUST NOT rely on
 *     the order, but a stable order helps diffing on the device side.
 *   - Bus arrays are placeholders in the v1 scaffold — every bus
 *     entry is materialised by a subsystem module once it comes online
 *     (i2c/spi/gpio/…). For now we advertise capabilities only, not
 *     concrete bus instances.
 */

#include "capabilities.h"
#include "rpbp/rpbp_cbor.h"
#include "version.h"

#include "pico/unique_id.h"

#include <string.h>

/* ───── Serial number helper ─────────────────────────────────────────── */

static void get_serial_bytes(uint8_t out[PICO_UNIQUE_BOARD_ID_SIZE_BYTES])
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    memcpy(out, id.id, PICO_UNIQUE_BOARD_ID_SIZE_BYTES);
}

/* ───── Identity sub-map (also used by GET_IDENTITY) ─────────────────── */

static void emit_identity_common(struct cbor_writer *w)
{
    uint8_t serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
    get_serial_bytes(serial);

    /* proto: [MAJOR, MINOR, PATCH] */
    cbor_writer_text_cstr(w, "proto");
    cbor_writer_array(w, 3);
    cbor_writer_uint(w, RPBP_VERSION_MAJOR);
    cbor_writer_uint(w, RPBP_VERSION_MINOR);
    cbor_writer_uint(w, RPBP_VERSION_PATCH);

    cbor_writer_kv_text(w, "fw",     RPBRIDGE_GIT_DESCRIBE);
    cbor_writer_kv_text(w, "board",  "rpbridge-rev-B");
    cbor_writer_kv_bytes(w, "serial", serial, sizeof(serial));
}

/* ───── GET_IDENTITY (compact) ───────────────────────────────────────── */

size_t rpbridge_identity_emit(uint8_t *buf, size_t cap)
{
    struct cbor_writer w;
    cbor_writer_init(&w, buf, cap);

    /* Map: proto, fw, board, serial — 4 entries. */
    cbor_writer_map(&w, 4);
    emit_identity_common(&w);

    return cbor_writer_ok(&w) ? cbor_writer_used(&w) : 0;
}

/* ───── Full capability map ──────────────────────────────────────────── */

#define RPBRIDGE_FEATURE_COUNT  17

static const char *const k_features[RPBRIDGE_FEATURE_COUNT] = {
    "cbor",
    "credit-fc",
    "events",
    "time-sync",
    "crc32c.sw",           /* software CRC for v1; hw DMA SNIFFER path pending */
    "firmware-update",
    "uart.ttl",            /* both uart instances can drive TTL                */
    "uart.rs232",          /* UART0 phy-bank: MAX3232                          */
    "uart.rs485",          /* UART0 phy-bank: SN65HVD72 + software DE          */
    "uart.pio.8n1-only",   /* PIO-UART framing is hard-wired to 8N1            */
    "i2c.100k",            /* I²C Standard-mode                                */
    "i2c.400k",            /* I²C Fast-mode                                    */
    "i2c.1m",              /* I²C Fast-mode Plus                               */
    "spi.mode-0-3",        /* SPI modes 0..3 (CPOL/CPHA)                       */
    "gpio.mask-atomic",    /* 64-bit atomic bank write/read                    */
    "pwm.invert",          /* PWM polarity inversion                           */
    "adc.temp",            /* on-chip temperature sensor                       */
};

size_t rpbridge_capabilities_emit(uint8_t *buf, size_t cap)
{
    struct cbor_writer w;
    cbor_writer_init(&w, buf, cap);

    /*
     * Top-level map entries (update the count any time you add a key):
     *   proto, fw, board, serial       (4 — from emit_identity_common)
     *   mtu                             (1)
     *   max_msg_len                     (1)
     *   max_streams                     (1)
     *   max_reassembled                 (1)
     *   features                        (1)
     *   buses                           (1)
     *   ota                             (1)
     *                           total = 11
     */
    cbor_writer_map(&w, 11);
    emit_identity_common(&w);

    /* mtu: { out: 64, in: 64 }  — USB FS bulk packet size */
    cbor_writer_text_cstr(&w, "mtu");
    cbor_writer_map(&w, 2);
    cbor_writer_kv_uint(&w, "out", 64);
    cbor_writer_kv_uint(&w, "in",  64);

    cbor_writer_kv_uint(&w, "max_msg_len",    4096);
    cbor_writer_kv_uint(&w, "max_streams",    16);
    cbor_writer_kv_uint(&w, "max_reassembled", 64 * 1024);

    /* features: string array */
    cbor_writer_text_cstr(&w, "features");
    cbor_writer_array(&w, RPBRIDGE_FEATURE_COUNT);
    for (size_t i = 0; i < RPBRIDGE_FEATURE_COUNT; ++i) {
        cbor_writer_text_cstr(&w, k_features[i]);
    }

    /*
     * buses: one entry per bus family, each a flat array of host-
     * visible instances. For UART we advertise one entry per CDC-ACM
     * — three share the UART0 HW backend (one per phy in the phy-
     * bank) so a host can pick the physical layer by opening the
     * correctly-named virtual COM port without ever touching RPBP.
     * The firmware auto-selects the transceiver from whichever CDC
     * has DTR asserted (see docs/protocol/subsystems/uart.md).
     *
     * Pending: SPI, GPIO, PWM, ADC, CAN, 1-Wire.
     */
    cbor_writer_text_cstr(&w, "buses");
    cbor_writer_map(&w, 6);

    cbor_writer_text_cstr(&w, "uart");
    cbor_writer_array(&w, 4);

    /* CDC #0 — UART0 HW via MAX3232 (RS232). */
    {
        cbor_writer_map(&w, 5);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "cdc",      0);
        cbor_writer_kv_text(&w, "backend",  "hw");
        cbor_writer_kv_text(&w, "phy",      "rs232");
        cbor_writer_kv_uint(&w, "max_baud", 3000000);
    }

    /* CDC #1 — UART0 HW via SN65HVD72 (RS485, auto DE on GP6). */
    {
        cbor_writer_map(&w, 6);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "cdc",      1);
        cbor_writer_kv_text(&w, "backend",  "hw");
        cbor_writer_kv_text(&w, "phy",      "rs485");
        cbor_writer_kv_uint(&w, "max_baud", 3000000);
        cbor_writer_kv_uint(&w, "de_pin",   6);
    }

    /* CDC #2 — UART0 HW direct-drive TTL (header #1). */
    {
        cbor_writer_map(&w, 5);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "cdc",      2);
        cbor_writer_kv_text(&w, "backend",  "hw");
        cbor_writer_kv_text(&w, "phy",      "ttl");
        cbor_writer_kv_uint(&w, "max_baud", 3000000);
    }

    /* CDC #3 — PIO-UART on GP33/GP36 (TTL-only, header #2).
     * Carries an explicit `framing` array so hosts don't have to rely
     * on the `"uart.pio.8n1-only"` feature flag alone — the PIO
     * programs are hard-wired to 8N1 and SetLineCoding requests for
     * other framings are clamped on-device (see uart_pio.h). */
    {
        cbor_writer_map(&w, 6);
        cbor_writer_kv_uint(&w, "idx",      1);
        cbor_writer_kv_uint(&w, "cdc",      3);
        cbor_writer_kv_text(&w, "backend",  "pio");
        cbor_writer_kv_text(&w, "phy",      "ttl");
        cbor_writer_kv_uint(&w, "max_baud", 3000000);
        cbor_writer_text_cstr(&w, "framing");
        cbor_writer_array(&w, 1);
        cbor_writer_text_cstr(&w, "8n1");
    }

    /*
     * I²C — two HW-backed buses on Qwiic headers + a 4-pin
     * break-out. Each entry carries the canonical freq ladder
     * rather than a single max_freq so hosts can pick a clock the
     * slaves on that bus can actually sustain.
     */
    cbor_writer_text_cstr(&w, "i2c");
    cbor_writer_array(&w, 2);

    /* I²C #0 — Qwiic 1+2 + header on GP2/GP3. */
    {
        cbor_writer_map(&w, 5);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "sda_pin",  2);
        cbor_writer_kv_uint(&w, "scl_pin",  3);
        cbor_writer_kv_uint(&w, "max_freq", 1000000);
        cbor_writer_text_cstr(&w, "freqs");
        cbor_writer_array(&w, 3);
        cbor_writer_uint(&w, 100000);
        cbor_writer_uint(&w, 400000);
        cbor_writer_uint(&w, 1000000);
    }

    /* I²C #1 — Qwiic 3 + header on GP34/GP35. */
    {
        cbor_writer_map(&w, 5);
        cbor_writer_kv_uint(&w, "idx",      1);
        cbor_writer_kv_uint(&w, "sda_pin",  34);
        cbor_writer_kv_uint(&w, "scl_pin",  35);
        cbor_writer_kv_uint(&w, "max_freq", 1000000);
        cbor_writer_text_cstr(&w, "freqs");
        cbor_writer_array(&w, 3);
        cbor_writer_uint(&w, 100000);
        cbor_writer_uint(&w, 400000);
        cbor_writer_uint(&w, 1000000);
    }

    /* SPI — two HW-backed buses with their canonical pin layout. */
    cbor_writer_text_cstr(&w, "spi");
    cbor_writer_array(&w, 2);

    /* SPI #0 — PMOD (single CS on GP17, user IRQ on GP9). */
    {
        cbor_writer_map(&w, 7);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "sck_pin",  18);
        cbor_writer_kv_uint(&w, "mosi_pin", 19);
        cbor_writer_kv_uint(&w, "miso_pin", 16);
        cbor_writer_kv_uint(&w, "max_freq", 62500000);
        cbor_writer_text_cstr(&w, "cs_pins");
        cbor_writer_array(&w, 1);
        cbor_writer_uint(&w, 17);
        cbor_writer_kv_uint(&w, "int_pin",  9);
    }
    /* SPI #1 — Breakout with 3 CS lines. */
    {
        cbor_writer_map(&w, 6);
        cbor_writer_kv_uint(&w, "idx",      1);
        cbor_writer_kv_uint(&w, "sck_pin",  10);
        cbor_writer_kv_uint(&w, "mosi_pin", 11);
        cbor_writer_kv_uint(&w, "miso_pin", 12);
        cbor_writer_kv_uint(&w, "max_freq", 62500000);
        cbor_writer_text_cstr(&w, "cs_pins");
        cbor_writer_array(&w, 3);
        cbor_writer_uint(&w, 13);
        cbor_writer_uint(&w, 14);
        cbor_writer_uint(&w, 15);
    }

    /* GPIO — single host-exposed bank, pins listed explicitly so the
     * host can pre-validate requests without a round-trip to LIST. */
    cbor_writer_text_cstr(&w, "gpio");
    cbor_writer_array(&w, 1);
    {
        cbor_writer_map(&w, 2);
        cbor_writer_kv_uint(&w, "idx",    0);
        cbor_writer_text_cstr(&w, "pins");
        cbor_writer_array(&w, 11);
        cbor_writer_uint(&w, 30); cbor_writer_uint(&w, 31);
        cbor_writer_uint(&w, 37); cbor_writer_uint(&w, 38);
        cbor_writer_uint(&w, 39); cbor_writer_uint(&w, 40);
        cbor_writer_uint(&w, 41); cbor_writer_uint(&w, 42);
        cbor_writer_uint(&w, 43); cbor_writer_uint(&w, 44);
        cbor_writer_uint(&w, 45);
    }

    /* PWM — user-exposed pins only (the LED channels are internal). */
    cbor_writer_text_cstr(&w, "pwm");
    cbor_writer_array(&w, 1);
    {
        cbor_writer_map(&w, 3);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "max_freq", 62500000);
        cbor_writer_text_cstr(&w, "pins");
        cbor_writer_array(&w, 6);
        cbor_writer_uint(&w, 30); cbor_writer_uint(&w, 31);
        cbor_writer_uint(&w, 37); cbor_writer_uint(&w, 38);
        cbor_writer_uint(&w, 39); cbor_writer_uint(&w, 40);
    }

    /* ADC — four user channels plus the on-chip temperature sensor. */
    cbor_writer_text_cstr(&w, "adc");
    cbor_writer_array(&w, 1);
    {
        cbor_writer_map(&w, 4);
        cbor_writer_kv_uint(&w, "idx",       0);
        cbor_writer_kv_uint(&w, "vref_mv",   3300);
        cbor_writer_kv_uint(&w, "resolution_bits", 12);
        cbor_writer_text_cstr(&w, "channels");
        cbor_writer_array(&w, 5);
        /* Each channel entry: [idx, gp_pin | -1 for internal, kind] */
        for (int i = 0; i < 4; ++i) {
            cbor_writer_map(&w, 3);
            cbor_writer_kv_uint(&w, "ch",     (uint64_t)i);
            cbor_writer_kv_uint(&w, "gp",     (uint64_t)(26 + i));
            cbor_writer_kv_text(&w, "kind",   "external");
        }
        cbor_writer_map(&w, 2);
        cbor_writer_kv_uint(&w, "ch",     4);
        cbor_writer_kv_text(&w, "kind",   "temperature");
    }

    /* ota descriptor */
    cbor_writer_text_cstr(&w, "ota");
    cbor_writer_map(&w, 2);
    cbor_writer_kv_text(&w, "mechanism", "uf2_bootsel");
    cbor_writer_kv_text(&w, "signature", "minisign-ed25519");

    return cbor_writer_ok(&w) ? cbor_writer_used(&w) : 0;
}
