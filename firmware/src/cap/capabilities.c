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

#define RPBRIDGE_FEATURE_COUNT  6

static const char *const k_features[RPBRIDGE_FEATURE_COUNT] = {
    "cbor",
    "credit-fc",
    "events",
    "time-sync",
    "crc32c.sw",        /* software CRC for v1; hw DMA SNIFFER path pending */
    "firmware-update",
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
     * buses: empty map in the v1 scaffold. As peripheral handlers come
     * online (SYS + I2C is the minimum viable; then SPI/GPIO/…), each
     * registers a per-bus descriptor and the count here updates.
     */
    cbor_writer_text_cstr(&w, "buses");
    cbor_writer_map(&w, 0);

    /* ota descriptor */
    cbor_writer_text_cstr(&w, "ota");
    cbor_writer_map(&w, 2);
    cbor_writer_kv_text(&w, "mechanism", "uf2_bootsel");
    cbor_writer_kv_text(&w, "signature", "minisign-ed25519");

    return cbor_writer_ok(&w) ? cbor_writer_used(&w) : 0;
}
