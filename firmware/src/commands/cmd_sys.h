/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * SYS subsystem handlers (subsys = 0). Implements the opcodes
 * documented in docs/protocol/subsystems/sys.md.
 */

#ifndef RPBRIDGE_CMD_SYS_H
#define RPBRIDGE_CMD_SYS_H

#include "rpbp/rpbp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Send a HELLO reply mirroring the request's channel/seq. Called by
 * the dispatcher when a host HELLO arrives. Returns 0 on success.
 */
int cmd_sys_reply_hello(const struct rpbp_header *req);

/*
 * Handle a CMD_REQUEST targeted at subsys SYS (0). Returns RPBP_OK
 * after it has sent a response frame; any non-OK value indicates an
 * internal dispatch error (e.g. TX failure).
 */
enum rpbp_status cmd_sys_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor);

/*
 * LED override: set R/G/B/mode/brightness from a host SET_LED
 * command. Returns 0 on success, non-zero on EINVAL (bad mode).
 * Implementation sits in the main loop so it can drive the PWM-
 * modulated LED state.
 */
int cmd_sys_set_led(uint8_t r, uint8_t g, uint8_t b,
                    uint8_t mode, uint8_t brightness_pct);

/*
 * Did the last SET_LED request leave an override active? The main
 * loop's heartbeat blinker checks this; when true, it stops
 * overwriting the LED and lets the override stand.
 */
bool cmd_sys_led_override_active(void);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_SYS_H */
