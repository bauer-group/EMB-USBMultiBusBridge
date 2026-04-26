/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * gs_usb protocol translator on vendor-class instance #1.
 *
 * Wire definitions come from mainline Linux
 * drivers/net/can/usb/gs_usb.c + include/linux/usb/gs_usb.h. We
 * implement the classic-CAN subset (plus HW_TIMESTAMP when the host
 * opts in via GS_CAN_MODE_HW_TIMESTAMP) that covers 100 % of
 * SocketCAN's needs on rev-B. CAN-FD is hardware-gated — see
 * docs/protocol/hardware-canfd.md and ADR-0006.
 */

#include "gs_usb_glue.h"

#include "peripherals/can.h"
#include "tusb.h"

#include "pico/time.h"   /* time_us_64() */

#include <string.h>

/* ───── gs_usb wire constants ────────────────────────────────────────── */

#define GS_USB_BREQ_HOST_FORMAT    0
#define GS_USB_BREQ_BITTIMING      1
#define GS_USB_BREQ_MODE           2
#define GS_USB_BREQ_BERR           3
#define GS_USB_BREQ_BT_CONST       4
#define GS_USB_BREQ_DEVICE_CONFIG  5
#define GS_USB_BREQ_TIMESTAMP      6
#define GS_USB_BREQ_IDENTIFY       7

#define GS_CAN_MODE_RESET          0u
#define GS_CAN_MODE_START          1u

/* gs_device_mode.flags (host → device): GS_CAN_MODE_* bitmask
 * that toggles optional behaviour at can_up time. Only the bits
 * we act on are named here; the rest are silently ignored. */
#define GS_CAN_MODE_LISTEN_ONLY            (1u << 0)
#define GS_CAN_MODE_LOOP_BACK              (1u << 1)
#define GS_CAN_MODE_TRIPLE_SAMPLE          (1u << 2)
#define GS_CAN_MODE_ONE_SHOT               (1u << 3)
#define GS_CAN_MODE_HW_TIMESTAMP           (1u << 4)
#define GS_CAN_MODE_PAD_PKTS_TO_MAX_PKT    (1u << 7)
#define GS_CAN_MODE_FD                     (1u << 8)
#define GS_CAN_MODE_BERR_REPORTING         (1u << 12)

/* Feature flags advertised in gs_device_bt_const.feature. A capability
 * flag says "the device can deliver this if the host enables it via
 * gs_device_mode.flags". Only IDENTIFY / USER_ID / HW_TIMESTAMP are
 * supported on rev-B; the rest are reserved for future silicon
 * (see ADR-0006). */
#define GS_CAN_FEATURE_LISTEN_ONLY         (1u << 0)
#define GS_CAN_FEATURE_LOOP_BACK           (1u << 1)
#define GS_CAN_FEATURE_TRIPLE_SAMPLE       (1u << 2)
#define GS_CAN_FEATURE_ONE_SHOT            (1u << 3)
#define GS_CAN_FEATURE_HW_TIMESTAMP        (1u << 4)
#define GS_CAN_FEATURE_IDENTIFY            (1u << 5)
#define GS_CAN_FEATURE_USER_ID             (1u << 6)
#define GS_CAN_FEATURE_PAD_PKTS_TO_MAX_PKT (1u << 7)
#define GS_CAN_FEATURE_FD                  (1u << 8)
#define GS_CAN_FEATURE_BERR_REPORTING      (1u << 12)

struct __attribute__((packed)) gs_device_config {
    uint8_t  reserved1;
    uint8_t  reserved2;
    uint8_t  reserved3;
    uint8_t  icount;        /* channels - 1 (0 = 1 channel)    */
    uint32_t sw_version;
    uint32_t hw_version;
};

struct __attribute__((packed)) gs_device_bt_const {
    uint32_t feature;
    uint32_t fclk_can;
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
};

struct __attribute__((packed)) gs_device_bittiming {
    uint32_t prop_seg;
    uint32_t phase_seg1;
    uint32_t phase_seg2;
    uint32_t sjw;
    uint32_t brp;
};

struct __attribute__((packed)) gs_device_mode {
    uint32_t mode;
    uint32_t flags;
};

/* gs_host_frame. Without HW_TIMESTAMP the wire size is 20 bytes
 * (12 header + 8 data). With HW_TIMESTAMP the host expects a 4-byte
 * little-endian µs counter appended right after the data payload,
 * making the frame 24 bytes. Mainline Linux computes the expected
 * frame size at interface up time and reads exactly that many
 * bytes per bulk packet. We store the wider form unconditionally
 * and send only the number of bytes the host actually asked for. */
struct __attribute__((packed)) gs_host_frame {
    uint32_t echo_id;
    uint32_t can_id;
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[8];
    uint32_t timestamp_us;   /* only on the wire when g_hw_ts_enabled */
};

#define GS_HOST_FRAME_SZ_CLASSIC    20u   /* without timestamp trailer */
#define GS_HOST_FRAME_SZ_HWTS       24u   /* with timestamp trailer    */

#define GS_USB_ECHO_ID_RX       0xFFFFFFFFu

/* ───── Our TinyUSB instance + config ────────────────────────────────── */

/* In our composite the RPBP vendor interface is vendor-class #0 and
 * the CAN (gs_usb) interface is vendor-class #1. See
 * src/usb/tusb_config.h → CFG_TUD_VENDOR = 2. */
#define VENDOR_IF_CAN        1u

#define GS_USB_FCLK_CAN      125000000u   /* can2040 uses clk_sys (150 MHz) but advertises 125 MHz baseline so the Linux CAN bit-timing calculator matches standard CAN silicon (MCP2515, mcp251x). */

/* ───── RX ring (from IRQ → poll) ────────────────────────────────────── */

#define GS_RX_RING_SLOTS     32u
_Static_assert((GS_RX_RING_SLOTS & (GS_RX_RING_SLOTS - 1u)) == 0,
               "ring size must be a power of two");

static struct gs_host_frame g_rx_ring[GS_RX_RING_SLOTS];
static volatile uint16_t    g_rx_head;
static volatile uint16_t    g_rx_tail;

static bool rx_ring_push(const struct gs_host_frame *f)
{
    uint16_t next = (uint16_t)((g_rx_head + 1u) & (GS_RX_RING_SLOTS - 1u));
    if (next == g_rx_tail) return false;      /* overflow — drop */
    g_rx_ring[g_rx_head] = *f;
    g_rx_head = next;
    return true;
}

static bool rx_ring_pop(struct gs_host_frame *f)
{
    if (g_rx_tail == g_rx_head) return false;
    *f = g_rx_ring[g_rx_tail];
    g_rx_tail = (uint16_t)((g_rx_tail + 1u) & (GS_RX_RING_SLOTS - 1u));
    return true;
}

/* ───── TX echo-id FIFO (max 4 in-flight — matches can2040 queue) ────── */

#define GS_TX_ECHO_SLOTS  4u
static volatile uint8_t  g_echo_head, g_echo_tail;
static volatile uint32_t g_echo_ids[GS_TX_ECHO_SLOTS];

static void echo_push(uint32_t id)
{
    uint8_t next = (uint8_t)((g_echo_head + 1u) & (GS_TX_ECHO_SLOTS - 1u));
    if (next == g_echo_tail) return;   /* full — drop the oldest */
    g_echo_ids[g_echo_head] = id;
    g_echo_head = next;
}

static bool echo_pop(uint32_t *id)
{
    if (g_echo_tail == g_echo_head) return false;
    *id = g_echo_ids[g_echo_tail];
    g_echo_tail = (uint8_t)((g_echo_tail + 1u) & (GS_TX_ECHO_SLOTS - 1u));
    return true;
}

/* Host-enabled HW timestamp trailer. The bit is set by the host in
 * gs_device_mode.flags during BREQ_MODE(START). Cleared on RESET. */
static volatile bool g_hw_ts_enabled;

#if defined(RPBRIDGE_HAVE_CANFD) && RPBRIDGE_HAVE_CANFD
/* CAN-FD is a compile-time gate because the physical layer on rev-B
 * does not support FD frames (can2040 is Classic-only). Defining
 * RPBRIDGE_HAVE_CANFD on a future board with an MCP2518FD (or similar)
 * daughterboard flips the frame size calculator to FD widths. The
 * protocol-layer additions needed for FD are documented in
 * docs/protocol/hardware-canfd.md. */
#  error "CAN-FD build selected but no FD-capable backend is wired up \
in src/peripherals/can.c. Replace can2040 with an FD controller or \
undefine RPBRIDGE_HAVE_CANFD."
#endif

static inline uint32_t host_frame_size(void)
{
    return g_hw_ts_enabled ? GS_HOST_FRAME_SZ_HWTS : GS_HOST_FRAME_SZ_CLASSIC;
}

/* ───── can2040 → gs_usb frame translation (called from PIO IRQ) ─────── */

static void can_rx_cb(uint32_t notify, const struct can_hw_frame *msg)
{
    struct gs_host_frame f = {0};
    f.can_id  = msg->id;
    f.can_dlc = (uint8_t)msg->dlc;
    f.channel = 0u;
    memcpy(f.data, msg->data, sizeof(f.data));
    /* time_us_64() on RP2350 is IRQ-safe: it reads the TIMER peripheral's
     * atomic 64-bit snapshot register. Truncating to 32 bits gives us
     * ~71.6 min of unambiguous range — matches mainline gs_usb, which
     * also advertises a 32-bit µs counter. */
    f.timestamp_us = (uint32_t)time_us_64();

    if (notify & CAN_HW_NOTIFY_TX) {
        /* TX-complete: echo the frame with the host-assigned echo_id
         * so SocketCAN can confirm the skb and release the queue.
         * Pop into a stack temporary first — `gs_host_frame` is
         * packed, so `&f.echo_id` is potentially unaligned and gcc
         * (correctly) refuses to hand its address to a uint32_t*. */
        uint32_t echo_id = 0u;
        (void)echo_pop(&echo_id);
        f.echo_id = echo_id;
        rx_ring_push(&f);
    } else if (notify & CAN_HW_NOTIFY_RX) {
        f.echo_id = GS_USB_ECHO_ID_RX;
        rx_ring_push(&f);
    }
    /* Error notifications land on GS_USB_BREQ_BERR — not wired up in
     * this first cut; can2040 already logs them via stats. */
}

/* ───── Control requests ─────────────────────────────────────────────── */

static uint32_t g_host_format = 0x0000beefu;   /* le32 sentinel */

static bool handle_control_setup(uint8_t rhport, void const *req_void)
{
    tusb_control_request_t const *req = req_void;

    /*
     * gs_usb class requests come in as standard vendor-class setup
     * packets on interface index 7 (our USB interface number for the
     * CAN vendor interface). `wIndex` carries the interface number.
     */
    if ((req->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) ||
        (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE))
        return false;

    switch (req->bRequest) {
    case GS_USB_BREQ_HOST_FORMAT:
        /* Host writes its byte-order sentinel; we accept either
         * direction with a small copy. */
        if (req->bmRequestType_bit.direction == TUSB_DIR_IN) {
            return tud_control_xfer(rhport, req,
                                    &g_host_format, sizeof(g_host_format));
        }
        return tud_control_xfer(rhport, req,
                                (void *)&g_host_format, sizeof(g_host_format));

    case GS_USB_BREQ_DEVICE_CONFIG: {
        static struct gs_device_config cfg = {
            .icount      = 0,                 /* 1 channel */
            .sw_version  = 2u,
            .hw_version  = 1u,
        };
        return tud_control_xfer(rhport, req, &cfg, sizeof(cfg));
    }

    case GS_USB_BREQ_BT_CONST: {
        /*
         * Advertise a CAN bit-timing window the host can use with
         * Linux's `can_update_spt()`. Values mirror mcp251x-ish
         * ranges and are sized so can2040 can be configured at any
         * standard automotive rate (100k / 125k / 250k / 500k / 1M).
         *
         * Advertising HW_TIMESTAMP tells mainline gs_usb to read
         * the 4-byte µs trailer from every frame (see struct
         * gs_host_frame_hw_timestamp in mainline). The host still
         * has to opt in at can_up time via GS_CAN_MODE_HW_TIMESTAMP
         * in gs_device_mode.flags — until then we keep sending
         * 20-byte classic frames.
         */
        static struct gs_device_bt_const bt = {
            .feature     = GS_CAN_FEATURE_IDENTIFY |
                           GS_CAN_FEATURE_USER_ID  |
                           GS_CAN_FEATURE_HW_TIMESTAMP,
            .fclk_can    = GS_USB_FCLK_CAN,
            .tseg1_min   = 1,
            .tseg1_max   = 16,
            .tseg2_min   = 1,
            .tseg2_max   = 8,
            .sjw_max     = 4,
            .brp_min     = 1,
            .brp_max     = 1024,
            .brp_inc     = 1,
        };
        return tud_control_xfer(rhport, req, &bt, sizeof(bt));
    }

    case GS_USB_BREQ_BITTIMING: {
        static struct gs_device_bittiming bt_buf;
        if (req->bmRequestType_bit.direction == TUSB_DIR_OUT) {
            bool ok = tud_control_xfer(rhport, req,
                                       &bt_buf, sizeof(bt_buf));
            if (ok) {
                /* Derive bitrate from the host's requested segments:
                 * bit_time = tq * (1 + prop + seg1 + seg2),
                 * bitrate  = fclk / (brp * bit_time). */
                uint32_t bits_per_tq = 1u + bt_buf.prop_seg +
                                       bt_buf.phase_seg1 + bt_buf.phase_seg2;
                uint32_t brp = bt_buf.brp ? bt_buf.brp : 1u;
                uint32_t bps = (bits_per_tq > 0u)
                    ? (GS_USB_FCLK_CAN / (brp * bits_per_tq))
                    : 0u;
                if (bps > 0u) can_hw_set_bitrate(bps);
            }
            return ok;
        }
        return false;
    }

    case GS_USB_BREQ_MODE: {
        static struct gs_device_mode mode_buf;
        if (req->bmRequestType_bit.direction == TUSB_DIR_OUT) {
            bool ok = tud_control_xfer(rhport, req,
                                       &mode_buf, sizeof(mode_buf));
            if (ok) {
                if (mode_buf.mode == GS_CAN_MODE_START) {
                    /* Latch per-session opt-ins. HW_TIMESTAMP is the only
                     * optional bit we honour today; the rest we advertise
                     * as supported in BT_CONST only if silicon exists. */
                    g_hw_ts_enabled = (mode_buf.flags & GS_CAN_MODE_HW_TIMESTAMP) != 0u;
                    can_hw_start();
                } else if (mode_buf.mode == GS_CAN_MODE_RESET) {
                    g_hw_ts_enabled = false;
                    can_hw_stop();
                }
            }
            return ok;
        }
        return false;
    }

    case GS_USB_BREQ_IDENTIFY:
        /* Host asks us to blink the LED. Acknowledge only. */
        return tud_control_xfer(rhport, req, NULL, 0);

    case GS_USB_BREQ_TIMESTAMP: {
        /* Host clock-anchor read: IN request returning the current
         * device µs counter as LE u32. Mainline gs_usb uses this to
         * compute an initial offset before relying on per-frame
         * timestamps. Wraps every ~71.6 min (32-bit µs). */
        if (req->bmRequestType_bit.direction != TUSB_DIR_IN)
            return false;
        static uint32_t ts_buf;
        ts_buf = (uint32_t)time_us_64();
        return tud_control_xfer(rhport, req, &ts_buf, sizeof(ts_buf));
    }

    case GS_USB_BREQ_BERR:
    default:
        return false;
    }
}

/* ───── Public glue API ──────────────────────────────────────────────── */

void gs_usb_glue_init(void)
{
    g_rx_head = g_rx_tail = 0u;
    g_echo_head = g_echo_tail = 0u;
    g_hw_ts_enabled = false;
    can_hw_set_rx_cb(can_rx_cb);
}

bool gs_usb_glue_control_xfer(uint8_t rhport, uint8_t stage,
                              void const *request)
{
    if (stage != CONTROL_STAGE_SETUP) return true;
    return handle_control_setup(rhport, request);
}

/*
 * Pump pending RX frames out the vendor IN pipe and service any bulk
 * OUT frames sent by the host.
 *
 * Called from the main loop alongside tud_task(). Must be non-
 * blocking and short — misbehaving here will starve RPBP.
 */
void gs_usb_glue_poll(void)
{
    if (!tud_vendor_n_mounted(VENDOR_IF_CAN)) return;

    const uint32_t wire_sz = host_frame_size();

    /* 1. Drain RX ring into the IN FIFO — one frame per poll keeps
     *    the latency reasonable without starving other subsystems. */
    struct gs_host_frame out;
    if (rx_ring_pop(&out)) {
        uint32_t free_bytes = tud_vendor_n_write_available(VENDOR_IF_CAN);
        if (free_bytes >= wire_sz) {
            tud_vendor_n_write(VENDOR_IF_CAN, &out, wire_sz);
            tud_vendor_n_write_flush(VENDOR_IF_CAN);
        } else {
            /* No room — re-queue at the tail so we don't lose it.
             * Safe because this poll is the only consumer. */
            g_rx_tail = (uint16_t)((g_rx_tail - 1u) & (GS_RX_RING_SLOTS - 1u));
            g_rx_ring[g_rx_tail] = out;
        }
    }

    /* 2. Consume bulk OUT frames from the host. The wire size is the
     *    same in both directions for a given mode — mainline gs_usb
     *    always sends one complete frame per bulk packet. */
    while (tud_vendor_n_available(VENDOR_IF_CAN) >= wire_sz) {
        struct gs_host_frame fi = {0};
        uint32_t got = tud_vendor_n_read(VENDOR_IF_CAN, &fi, wire_sz);
        if (got != wire_sz) break;

        struct can_hw_frame tx = {
            .id  = fi.can_id,
            .dlc = fi.can_dlc,
        };
        memcpy(tx.data, fi.data, sizeof(tx.data));

        echo_push(fi.echo_id);
        int rc = can_hw_transmit(&tx);
        if (rc < 0) {
            /* TX queue full — drop the echo-id (we won't get a
             * NOTIFY_TX for this frame) so the FIFO doesn't drift. */
            uint32_t drop;
            echo_pop(&drop);
        }
        /* Host-supplied timestamp (fi.timestamp_us when HW_TS is on)
         * is intentionally ignored: it is echo-only in gs_usb and we
         * don't need it — the TX-complete frame we emit back will
         * carry our own device timestamp, which is what SocketCAN
         * correlates against. */
    }
}
