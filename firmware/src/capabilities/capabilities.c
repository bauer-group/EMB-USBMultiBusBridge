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

#include "pico.h"             /* board feature flags */
#include "pico/unique_id.h"

#include <string.h>

/* ───── Board identity strings (from board header) ───────────────────── */

#ifndef RPBRIDGE_BOARD_NAME
#  define RPBRIDGE_BOARD_NAME  "rpbridge-rev-B"
#endif

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
    cbor_writer_kv_text(w, "board",  RPBRIDGE_BOARD_NAME);
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

/* ───── Full capability map ──────────────────────────────────────────── *
 *
 * Feature list, bus list, and per-bus entries are all variant-driven
 * via the active board header's RPBRIDGE_HAVE_* flags. The CBOR
 * arrays carry definite lengths so we compute counts at compile time
 * and emit the matching number of entries.
 */

/* Optional features — toggled by the board header. */
#define FEAT_UART_RS232      RPBRIDGE_HAVE_RS232_PHY
#define FEAT_UART_RS485      RPBRIDGE_HAVE_RS485_PHY
#define FEAT_UART_PIO_8N1    RPBRIDGE_HAVE_PIO_UART
#define FEAT_HAVE_CAN        RPBRIDGE_HAVE_CAN
#define FEAT_HAVE_I2C1       RPBRIDGE_HAVE_I2C1
#define FEAT_HAVE_SPI1       RPBRIDGE_HAVE_SPI1

/* Always-on features count. Whenever you add or remove a string in
 * the always-on block of `rpbridge_capabilities_emit`, update this
 * constant — the CBOR array length must match. */
#define RPBRIDGE_BASE_FEATURE_COUNT  15u

/* Counted at compile time so cbor_writer_array() gets the right N. */
#define RPBRIDGE_FEATURE_COUNT (                                  \
        RPBRIDGE_BASE_FEATURE_COUNT                                \
      + (FEAT_UART_RS232    ? 1 : 0)                              \
      + (FEAT_UART_RS485    ? 1 : 0)                              \
      + (FEAT_UART_PIO_8N1  ? 1 : 0)                              \
      + (FEAT_HAVE_CAN      ? 3 : 0)   /* classic + gs-usb + hw-ts */ \
)

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
     *   access                          (1)   — ADR-0005
     *   ota                             (1)
     *                           total = 12
     */
    cbor_writer_map(&w, 12);
    emit_identity_common(&w);

    /* mtu: { out: 64, in: 64 }  — USB FS bulk packet size */
    cbor_writer_text_cstr(&w, "mtu");
    cbor_writer_map(&w, 2);
    cbor_writer_kv_uint(&w, "out", 64);
    cbor_writer_kv_uint(&w, "in",  64);

    cbor_writer_kv_uint(&w, "max_msg_len",    4096);
    cbor_writer_kv_uint(&w, "max_streams",    16);
    cbor_writer_kv_uint(&w, "max_reassembled", 64 * 1024);

    /* features: string array, board-flag driven. */
    cbor_writer_text_cstr(&w, "features");
    cbor_writer_array(&w, RPBRIDGE_FEATURE_COUNT);

    /* Always-on (RPBRIDGE_BASE_FEATURE_COUNT = 15) — protocol-stack
     * capabilities + universally-supported subsystem features. Order
     * intentional, mirrors the docs/protocol/capabilities.md
     * "feature ladder". Update RPBRIDGE_BASE_FEATURE_COUNT in
     * lock-step with any add/remove here. */
    cbor_writer_text_cstr(&w, "cbor");                      /*  1 */
    cbor_writer_text_cstr(&w, "credit-fc");                 /*  2 */
    cbor_writer_text_cstr(&w, "events");                    /*  3 */
    cbor_writer_text_cstr(&w, "time-sync");                 /*  4 */
    cbor_writer_text_cstr(&w, "crc32c.sw");                 /*  5 */
    cbor_writer_text_cstr(&w, "firmware-update");           /*  6 */
    cbor_writer_text_cstr(&w, "uart.ttl");                  /*  7 */
    cbor_writer_text_cstr(&w, "i2c.100k");                  /*  8 */
    cbor_writer_text_cstr(&w, "i2c.400k");                  /*  9 */
    cbor_writer_text_cstr(&w, "i2c.1m");                    /* 10 */
    cbor_writer_text_cstr(&w, "spi.mode-0-3");              /* 11 */
    cbor_writer_text_cstr(&w, "gpio.mask-atomic");          /* 12 */
    cbor_writer_text_cstr(&w, "pwm.invert");                /* 13 */
    cbor_writer_text_cstr(&w, "adc.temp");                  /* 14 */
    cbor_writer_text_cstr(&w, "adc.stream");                /* 15 */

    /* Optional features (board-flag gated). */
#if FEAT_UART_RS232
    cbor_writer_text_cstr(&w, "uart.rs232");
#endif
#if FEAT_UART_RS485
    cbor_writer_text_cstr(&w, "uart.rs485");
#endif
#if FEAT_UART_PIO_8N1
    cbor_writer_text_cstr(&w, "uart.pio.8n1-only");
#endif
#if FEAT_HAVE_CAN
    cbor_writer_text_cstr(&w, "can.classic");
    cbor_writer_text_cstr(&w, "can.gs-usb");
    cbor_writer_text_cstr(&w, "can.hw-timestamp");
#endif

    /*
     * buses: one entry per bus family, each a flat array of host-
     * visible instances. UART entries reflect what's actually
     * compiled in: TTL on UART0 HW is universal, RS232/RS485 require
     * the populated MAX3232/SN65HVD72 (RPBRIDGE_HAVE_RS232/485_PHY),
     * and the 4th CDC (PIO-UART) requires RPBRIDGE_HAVE_PIO_UART.
     */
    cbor_writer_text_cstr(&w, "buses");
    cbor_writer_map(&w, 7);

#define UART_BUS_COUNT (1 /* TTL */                          \
                      + (FEAT_UART_RS232    ? 1 : 0)         \
                      + (FEAT_UART_RS485    ? 1 : 0)         \
                      + (FEAT_UART_PIO_8N1  ? 1 : 0))

    cbor_writer_text_cstr(&w, "uart");
    cbor_writer_array(&w, UART_BUS_COUNT);

    /* CDC index assignment: phys come first in fixed order
     * (RS232=0, RS485=1, TTL=2 OR if no phys, TTL=0), then
     * PIO-UART last. Matches what cdc_glue.c expects. */
    {
        unsigned cdc_idx = 0;
#if FEAT_UART_RS232
        cbor_writer_map(&w, 5);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "cdc",      cdc_idx++);
        cbor_writer_kv_text(&w, "backend",  "hw");
        cbor_writer_kv_text(&w, "phy",      "rs232");
        cbor_writer_kv_uint(&w, "max_baud", 3000000);
#endif
#if FEAT_UART_RS485
        cbor_writer_map(&w, 6);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "cdc",      cdc_idx++);
        cbor_writer_kv_text(&w, "backend",  "hw");
        cbor_writer_kv_text(&w, "phy",      "rs485");
        cbor_writer_kv_uint(&w, "max_baud", 3000000);
        cbor_writer_kv_uint(&w, "de_pin",   RPBRIDGE_RS485_DE_PIN);
#endif
        /* TTL on UART0 HW — every variant has this. */
        cbor_writer_map(&w, 5);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "cdc",      cdc_idx++);
        cbor_writer_kv_text(&w, "backend",  "hw");
        cbor_writer_kv_text(&w, "phy",      "ttl");
        cbor_writer_kv_uint(&w, "max_baud", 3000000);
#if FEAT_UART_PIO_8N1
        /* PIO-UART (4th CDC). Carries an explicit `framing` array
         * because the PIO programs are hard-wired to 8N1 and
         * SetLineCoding requests for other framings are clamped
         * on-device (see uart_pio.h). */
        cbor_writer_map(&w, 6);
        cbor_writer_kv_uint(&w, "idx",      1);
        cbor_writer_kv_uint(&w, "cdc",      cdc_idx++);
        cbor_writer_kv_text(&w, "backend",  "pio");
        cbor_writer_kv_text(&w, "phy",      "ttl");
        cbor_writer_kv_uint(&w, "max_baud", 3000000);
        cbor_writer_text_cstr(&w, "framing");
        cbor_writer_array(&w, 1);
        cbor_writer_text_cstr(&w, "8n1");
#endif
        (void)cdc_idx;
    }

    /*
     * I²C — bus 0 universally; bus 1 only on RP2350B (board-gated).
     */
#define I2C_BUS_COUNT (1 + (FEAT_HAVE_I2C1 ? 1 : 0))
    cbor_writer_text_cstr(&w, "i2c");
    cbor_writer_array(&w, I2C_BUS_COUNT);

    /* I²C #0 — Qwiic 1+2 + header. */
    {
        cbor_writer_map(&w, 5);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "sda_pin",  RPBRIDGE_I2C0_SDA_PIN);
        cbor_writer_kv_uint(&w, "scl_pin",  RPBRIDGE_I2C0_SCL_PIN);
        cbor_writer_kv_uint(&w, "max_freq", 1000000);
        cbor_writer_text_cstr(&w, "freqs");
        cbor_writer_array(&w, 3);
        cbor_writer_uint(&w, 100000);
        cbor_writer_uint(&w, 400000);
        cbor_writer_uint(&w, 1000000);
    }

#if FEAT_HAVE_I2C1
    /* I²C #1 — Qwiic 3 + header (rev-B only). */
    {
        cbor_writer_map(&w, 5);
        cbor_writer_kv_uint(&w, "idx",      1);
        cbor_writer_kv_uint(&w, "sda_pin",  RPBRIDGE_I2C1_SDA_PIN);
        cbor_writer_kv_uint(&w, "scl_pin",  RPBRIDGE_I2C1_SCL_PIN);
        cbor_writer_kv_uint(&w, "max_freq", 1000000);
        cbor_writer_text_cstr(&w, "freqs");
        cbor_writer_array(&w, 3);
        cbor_writer_uint(&w, 100000);
        cbor_writer_uint(&w, 400000);
        cbor_writer_uint(&w, 1000000);
    }
#endif

    /* SPI — bus 0 universally; bus 1 board-gated. */
#define SPI_BUS_COUNT (1 + (FEAT_HAVE_SPI1 ? 1 : 0))
    cbor_writer_text_cstr(&w, "spi");
    cbor_writer_array(&w, SPI_BUS_COUNT);

    /* SPI #0 — PMOD (single CS on PICO_DEFAULT_SPI_CSN_PIN). */
    {
        cbor_writer_map(&w, 6);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "sck_pin",  RPBRIDGE_SPI0_SCK_PIN);
        cbor_writer_kv_uint(&w, "mosi_pin", RPBRIDGE_SPI0_MOSI_PIN);
        cbor_writer_kv_uint(&w, "miso_pin", RPBRIDGE_SPI0_MISO_PIN);
        cbor_writer_kv_uint(&w, "max_freq", 62500000);
        cbor_writer_text_cstr(&w, "cs_pins");
        cbor_writer_array(&w, 1);
        cbor_writer_uint(&w, PICO_DEFAULT_SPI_CSN_PIN);
    }

#if FEAT_HAVE_SPI1
    /* SPI #1 — Breakout with 3 CS lines (rev-B only).
     * CS pins are board-defined for forward compatibility. */
#  ifndef RPBRIDGE_SPI1_CS0_PIN
#    define RPBRIDGE_SPI1_CS0_PIN  13u
#  endif
#  ifndef RPBRIDGE_SPI1_CS1_PIN
#    define RPBRIDGE_SPI1_CS1_PIN  14u
#  endif
#  ifndef RPBRIDGE_SPI1_CS2_PIN
#    define RPBRIDGE_SPI1_CS2_PIN  15u
#  endif
    {
        cbor_writer_map(&w, 6);
        cbor_writer_kv_uint(&w, "idx",      1);
        cbor_writer_kv_uint(&w, "sck_pin",  RPBRIDGE_SPI1_SCK_PIN);
        cbor_writer_kv_uint(&w, "mosi_pin", RPBRIDGE_SPI1_MOSI_PIN);
        cbor_writer_kv_uint(&w, "miso_pin", RPBRIDGE_SPI1_MISO_PIN);
        cbor_writer_kv_uint(&w, "max_freq", 62500000);
        cbor_writer_text_cstr(&w, "cs_pins");
        cbor_writer_array(&w, 3);
        cbor_writer_uint(&w, RPBRIDGE_SPI1_CS0_PIN);
        cbor_writer_uint(&w, RPBRIDGE_SPI1_CS1_PIN);
        cbor_writer_uint(&w, RPBRIDGE_SPI1_CS2_PIN);
    }
#endif

    /* GPIO — single host-exposed bank, pins listed from the board's
     * RPBRIDGE_USER_GPIO_PIN_LIST so hosts can pre-validate requests
     * without a LIST round-trip. */
    cbor_writer_text_cstr(&w, "gpio");
    cbor_writer_array(&w, 1);
    {
        static const uint8_t k_gpio_pins[RPBRIDGE_USER_GPIO_PIN_COUNT] =
            RPBRIDGE_USER_GPIO_PIN_LIST;
        cbor_writer_map(&w, 2);
        cbor_writer_kv_uint(&w, "idx",    0);
        cbor_writer_text_cstr(&w, "pins");
        cbor_writer_array(&w, RPBRIDGE_USER_GPIO_PIN_COUNT);
        for (size_t i = 0; i < RPBRIDGE_USER_GPIO_PIN_COUNT; ++i) {
            cbor_writer_uint(&w, k_gpio_pins[i]);
        }
    }

    /* PWM — user-exposed pins from RPBRIDGE_USER_PWM_PIN_LIST. */
    cbor_writer_text_cstr(&w, "pwm");
    cbor_writer_array(&w, 1);
    {
        static const uint8_t k_pwm_pins[RPBRIDGE_USER_PWM_PIN_COUNT] =
            RPBRIDGE_USER_PWM_PIN_LIST;
        cbor_writer_map(&w, 3);
        cbor_writer_kv_uint(&w, "idx",      0);
        cbor_writer_kv_uint(&w, "max_freq", 62500000);
        cbor_writer_text_cstr(&w, "pins");
        cbor_writer_array(&w, RPBRIDGE_USER_PWM_PIN_COUNT);
        for (size_t i = 0; i < RPBRIDGE_USER_PWM_PIN_COUNT; ++i) {
            cbor_writer_uint(&w, k_pwm_pins[i]);
        }
    }

    /* ADC — external channels from RPBRIDGE_ADC_CHN_PIN + on-chip
     * temperature sensor. */
    cbor_writer_text_cstr(&w, "adc");
    cbor_writer_array(&w, 1);
    {
        static const uint8_t k_adc_pins[RPBRIDGE_ADC_EXT_CHANNEL_COUNT] = {
            RPBRIDGE_ADC_CH0_PIN, RPBRIDGE_ADC_CH1_PIN,
            RPBRIDGE_ADC_CH2_PIN, RPBRIDGE_ADC_CH3_PIN,
        };
        cbor_writer_map(&w, 4);
        cbor_writer_kv_uint(&w, "idx",       0);
        cbor_writer_kv_uint(&w, "vref_mv",   3300);
        cbor_writer_kv_uint(&w, "resolution_bits", 12);
        cbor_writer_text_cstr(&w, "channels");
        cbor_writer_array(&w, RPBRIDGE_ADC_EXT_CHANNEL_COUNT + 1u);
        for (size_t i = 0; i < RPBRIDGE_ADC_EXT_CHANNEL_COUNT; ++i) {
            cbor_writer_map(&w, 3);
            cbor_writer_kv_uint(&w, "ch",   (uint64_t)i);
            cbor_writer_kv_uint(&w, "gp",   (uint64_t)k_adc_pins[i]);
            cbor_writer_kv_text(&w, "kind", "external");
        }
        cbor_writer_map(&w, 2);
        cbor_writer_kv_uint(&w, "ch",     RPBRIDGE_ADC_EXT_CHANNEL_COUNT);
        cbor_writer_kv_text(&w, "kind",   "temperature");
    }

    /* CAN — single bus via can2040 on PIO0 when present. Surfaced to
     * the host as a standard SocketCAN device over the gs_usb vendor
     * interface; the RPBP protocol does not carry CAN frames directly. */
#define CAN_BUS_COUNT (FEAT_HAVE_CAN ? 1 : 0)
    cbor_writer_text_cstr(&w, "can");
    cbor_writer_array(&w, CAN_BUS_COUNT);
#if FEAT_HAVE_CAN
    {
        cbor_writer_map(&w, 6);
        cbor_writer_kv_uint(&w, "idx",       0);
        cbor_writer_kv_uint(&w, "tx_pin",    RPBRIDGE_CAN_TX_PIN);
        cbor_writer_kv_uint(&w, "rx_pin",    RPBRIDGE_CAN_RX_PIN);
        cbor_writer_kv_text(&w, "backend",   "pio");
        cbor_writer_kv_text(&w, "transport", "gs_usb");
        cbor_writer_kv_uint(&w, "max_bitrate_bps", 1000000);
    }
#endif

    /*
     * access — where each subsystem is reachable from the host
     * (ADR-0005). Hosts use this to route requests to the right
     * endpoint without guessing:
     *   "cdc"    — inbox usbser-bound virtual COM port
     *   "vendor" — RPBP over our vendor interface (driver or libusb)
     *   "cdc+vendor" — reserved for future dual-reachable subsystems
     */
    cbor_writer_text_cstr(&w, "access");
    cbor_writer_map(&w, 7);
    cbor_writer_kv_text(&w, "uart", "cdc");
    cbor_writer_kv_text(&w, "i2c",  "vendor");
    cbor_writer_kv_text(&w, "spi",  "vendor");
    cbor_writer_kv_text(&w, "gpio", "vendor");
    cbor_writer_kv_text(&w, "pwm",  "vendor");
    cbor_writer_kv_text(&w, "adc",  "vendor");
    cbor_writer_kv_text(&w, "can",  "vendor");

    /* ota descriptor */
    cbor_writer_text_cstr(&w, "ota");
    cbor_writer_map(&w, 2);
    cbor_writer_kv_text(&w, "mechanism", "uf2_bootsel");
    cbor_writer_kv_text(&w, "signature", "minisign-ed25519");

    return cbor_writer_ok(&w) ? cbor_writer_used(&w) : 0;
}
