/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * CAN driver — thin wrapper around can2040.
 */

#include "can.h"
#include "pin_broker.h"
#include "rpbp/rpbp.h"

#if defined(RPBRIDGE_HAVE_CAN2040) && RPBRIDGE_HAVE_CAN2040
# include "can2040.h"
#endif

#include "hardware/irq.h"
#include "hardware/clocks.h"

#include <string.h>

/* Rev-B board pins. */
#define CAN_HW_TX_PIN   20u
#define CAN_HW_RX_PIN   21u
#define CAN_HW_PIO_NUM  0u     /* can2040 hard-codes one PIO block */

#if defined(RPBRIDGE_HAVE_CAN2040) && RPBRIDGE_HAVE_CAN2040

static struct can2040   g_cbus;
static uint32_t         g_bitrate;
static bool             g_running;
static can_hw_cb_t      g_user_cb;

/* ───── can2040 callback thunk — runs in PIO-IRQ context ─────────────── */

static void c2040_cb_thunk(struct can2040 *cd, uint32_t notify,
                           struct can2040_msg *msg)
{
    (void)cd;
    if (g_user_cb == NULL) return;

    struct can_hw_frame f;
    if (msg != NULL) {
        f.id  = msg->id;
        f.dlc = msg->dlc;
        memcpy(f.data, msg->data, sizeof(f.data));
    } else {
        memset(&f, 0, sizeof(f));
    }

    /* Translate can2040 notify bits (1<<20 / 21 / 23) to our stable
     * wire values so future can2040 updates don't leak through. */
    uint32_t n = 0u;
    if (notify & CAN2040_NOTIFY_RX)    n |= CAN_HW_NOTIFY_RX;
    if (notify & CAN2040_NOTIFY_TX)    n |= CAN_HW_NOTIFY_TX;
    if (notify & CAN2040_NOTIFY_ERROR) n |= CAN_HW_NOTIFY_ERROR;

    g_user_cb(n, &f);
}

/* PIO0 IRQ0 vector — forwarded verbatim into can2040. */
static void pio0_irq0_isr(void)
{
    can2040_pio_irq_handler(&g_cbus);
}

/* ───── Public API ───────────────────────────────────────────────────── */

int can_hw_init(uint32_t bitrate_bps)
{
    if (bitrate_bps == 0u) return -RPBP_EINVAL;

    if (pin_broker_claim(CAN_HW_TX_PIN, PIN_OWNER_CAN, PIN_FUNC_PIO) < 0)
        return -RPBP_EBUSY;
    if (pin_broker_claim(CAN_HW_RX_PIN, PIN_OWNER_CAN, PIN_FUNC_PIO) < 0)
        return -RPBP_EBUSY;

    can2040_setup(&g_cbus, CAN_HW_PIO_NUM);
    can2040_callback_config(&g_cbus, c2040_cb_thunk);

    /* can2040 hooks PIO0_IRQ_0 (shared with our IRQ number 7 on
     * RP2350). Install the handler and enable the NVIC line. */
    irq_set_exclusive_handler(PIO0_IRQ_0, pio0_irq0_isr);
    irq_set_enabled(PIO0_IRQ_0, true);

    can2040_start(&g_cbus, clock_get_hz(clk_sys), bitrate_bps,
                  (int32_t)CAN_HW_RX_PIN, (int32_t)CAN_HW_TX_PIN);

    g_bitrate = bitrate_bps;
    g_running = true;
    return 0;
}

void can_hw_set_rx_cb(can_hw_cb_t cb)
{
    g_user_cb = cb;
}

int can_hw_set_bitrate(uint32_t bitrate_bps)
{
    if (bitrate_bps == 0u) return -RPBP_EINVAL;
    if (!g_running) {
        g_bitrate = bitrate_bps;
        return 0;
    }
    can2040_stop(&g_cbus);
    can2040_start(&g_cbus, clock_get_hz(clk_sys), bitrate_bps,
                  (int32_t)CAN_HW_RX_PIN, (int32_t)CAN_HW_TX_PIN);
    g_bitrate = bitrate_bps;
    return 0;
}

uint32_t can_hw_bitrate(void)
{
    return g_bitrate;
}

int can_hw_transmit(const struct can_hw_frame *msg)
{
    if (msg == NULL)             return -RPBP_EINVAL;
    if (msg->dlc > 8u)           return -RPBP_EINVAL;
    if (!g_running)              return -RPBP_EAGAIN;

    struct can2040_msg m;
    m.id  = msg->id;
    m.dlc = msg->dlc;
    memcpy(m.data, msg->data, sizeof(m.data));

    int rc = can2040_transmit(&g_cbus, &m);
    /* can2040 returns 0 on success, <0 when queue is full. */
    if (rc < 0) return -RPBP_EAGAIN;
    return 0;
}

void can_hw_stop(void)
{
    if (!g_running) return;
    can2040_stop(&g_cbus);
    g_running = false;
}

int can_hw_start(void)
{
    if (g_running) return 0;
    if (g_bitrate == 0u) return -RPBP_EINVAL;

    can2040_start(&g_cbus, clock_get_hz(clk_sys), g_bitrate,
                  (int32_t)CAN_HW_RX_PIN, (int32_t)CAN_HW_TX_PIN);
    g_running = true;
    return 0;
}

void can_hw_stats(struct can_hw_stats *out)
{
    if (out == NULL) return;
    struct can2040_stats s;
    can2040_get_statistics(&g_cbus, &s);
    out->rx_total      = s.rx_total;
    out->tx_total      = s.tx_total;
    out->tx_attempt    = s.tx_attempt;
    out->parse_error   = s.parse_error;
    out->tx_queue_full = 0u;   /* can2040 doesn't expose this yet */
}

#else   /* !RPBRIDGE_HAVE_CAN2040 — submodule missing: no-op stub */

int      can_hw_init(uint32_t b)              { (void)b; return -RPBP_ENOTSUP; }
void     can_hw_set_rx_cb(can_hw_cb_t cb)     { (void)cb; }
int      can_hw_set_bitrate(uint32_t b)       { (void)b; return -RPBP_ENOTSUP; }
uint32_t can_hw_bitrate(void)                 { return 0u; }
int      can_hw_transmit(const struct can_hw_frame *m) { (void)m; return -RPBP_ENOTSUP; }
void     can_hw_stop(void)                    {}
int      can_hw_start(void)                   { return -RPBP_ENOTSUP; }
void     can_hw_stats(struct can_hw_stats *o) { if (o) memset(o, 0, sizeof(*o)); }

#endif
