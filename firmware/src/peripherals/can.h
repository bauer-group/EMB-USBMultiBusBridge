/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * can — RP2350 CAN 2.0B via the vendored can2040 software controller.
 *
 * can2040 implements the full CAN-2.0B controller in PIO + IRQ code;
 * we only need an external CAN transceiver (SN65HVD230 on rev-B) for
 * bus drive. The vendored submodule (firmware/third_party/can2040)
 * claims all 3 state machines of PIO0 — hence the PIO0 ownership
 * reservation in docs/firmware/resource-budget.md.
 *
 * Pin map (rev-B): TX = GP20, RX = GP21.
 * Default bitrate: 500 kbit/s (automotive default, easy to integrate).
 *
 * This header is the thin C API that cdc_glue / gs_usb_glue / userland
 * talk to. Frames come in from IRQ context via the registered RX
 * callback — keep that short: enqueue, don't block.
 */

#ifndef RPBRIDGE_CAN_H
#define RPBRIDGE_CAN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors the can2040_msg layout; re-declared here so callers don't
 * have to include can2040.h (keeps the compile surface clean). */
struct can_hw_frame {
    uint32_t id;      /* bit 31 = EFF, bit 30 = RTR, bits 0..10 / 0..28 = ID */
    uint32_t dlc;     /* 0..8                                                */
    uint8_t  data[8];
};

#define CAN_HW_FLAG_EFF   (1u << 31)
#define CAN_HW_FLAG_RTR   (1u << 30)

enum can_hw_notify {
    CAN_HW_NOTIFY_RX    = 1u << 0,   /* new frame received      */
    CAN_HW_NOTIFY_TX    = 1u << 1,   /* frame successfully sent */
    CAN_HW_NOTIFY_ERROR = 1u << 2,   /* bus-level error         */
};

/* Callback signature. Called from PIO IRQ context: MUST be short. */
typedef void (*can_hw_cb_t)(uint32_t notify, const struct can_hw_frame *msg);

/*
 * Bring the CAN controller up. Claims PIO0 (all 3 SMs), GP20/GP21 via
 * the pin broker, installs PIO0 IRQ handlers. Starts with the
 * requested bitrate (use can_hw_set_bitrate() to change later without
 * tearing down PIO state).
 *
 * Returns 0 on success, -RPBP_EBUSY on pin/PIO conflict, -RPBP_EINVAL
 * on zero bitrate.
 */
int can_hw_init(uint32_t bitrate_bps);

/* Register the RX/TX/error callback. May be called before or after
 * init. NULL unregisters. */
void can_hw_set_rx_cb(can_hw_cb_t cb);

/* Change the bus bitrate. Implicitly stops + restarts the controller;
 * in-flight TX frames in the can2040 queue are dropped. */
int can_hw_set_bitrate(uint32_t bitrate_bps);

uint32_t can_hw_bitrate(void);

/* Queue a frame for transmission. Returns 0 on success, -RPBP_EAGAIN
 * if the TX queue is full, -RPBP_EINVAL on a malformed frame. */
int can_hw_transmit(const struct can_hw_frame *msg);

/* Optional: stop the controller entirely (idle the bus, release PIO
 * but keep pin claims). Useful for gs_usb MODE=RESET. */
void can_hw_stop(void);

/* Restart after a stop. */
int can_hw_start(void);

struct can_hw_stats {
    uint32_t rx_total;
    uint32_t tx_total;
    uint32_t tx_attempt;
    uint32_t parse_error;
    uint32_t tx_queue_full;
};

void can_hw_stats(struct can_hw_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CAN_H */
