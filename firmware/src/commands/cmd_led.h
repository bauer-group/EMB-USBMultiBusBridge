/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * LED subsystem dispatcher (subsys = 7) — addressable LED strips
 * (WS2812 / SK6812 / WS2811) driven from a PIO state machine on PIO2.
 * See docs/protocol/subsystems/led.md for the wire format.
 */

#ifndef RPBRIDGE_CMD_LED_H
#define RPBRIDGE_CMD_LED_H

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rpbp_status cmd_led_dispatch(const struct rpbp_header *hdr,
                                  uint8_t opcode,
                                  const uint8_t *args,
                                  size_t args_len,
                                  bool cbor);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_LED_H */
