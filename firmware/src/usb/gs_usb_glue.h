/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * gs_usb_glue — Linux-compatible gs_usb protocol on USB vendor
 * interface 1.
 *
 * The gs_usb protocol is the wire format spoken by Geschwister-
 * Schneider CAN adapters (candleLight, cantact, etc.). Mainline Linux
 * has a driver (drivers/net/can/usb/gs_usb.c) that binds to any
 * device exposing the right VID/PID + vendor interface. By speaking
 * it we get SocketCAN support without writing a custom Linux driver
 * for the CAN bus — `ip link set can0 up type can bitrate 500000`
 * just works.
 *
 * Windows support: candle_api (https://github.com/HubertD/candle_api)
 * binds automatically via our MS OS 2.0 descriptors.
 */

#ifndef RPBRIDGE_GS_USB_GLUE_H
#define RPBRIDGE_GS_USB_GLUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time bring-up. Registers our can_hw RX callback and is safe to
 * call before tusb_init() — USB transfers are only initiated once the
 * host issues gs_usb control requests. */
void gs_usb_glue_init(void);

/* Poll from the main loop. Pumps pending RX frames from the internal
 * ring into the TinyUSB vendor IN FIFO and services host bulk-OUT
 * requests (frames arriving from userland → can_hw_transmit). */
void gs_usb_glue_poll(void);

/* TinyUSB control-transfer callback — dispatches gs_usb class
 * requests on vendor interface 1 (DEVICE_CONFIG, BT_CONST, MODE,
 * BITTIMING, ...). Returns false if the request was not consumed so
 * the fallback MS-OS-2.0 path (interface 8) still runs. */
bool gs_usb_glue_control_xfer(uint8_t rhport, uint8_t stage,
                              void const *request);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_GS_USB_GLUE_H */
