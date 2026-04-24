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
 * Snapshot of an active SET_LED override. Mirrors the wire fields of
 * the SET_LED opcode (see docs/protocol/subsystems/sys.md §0x05).
 */
struct cmd_sys_led_override {
    uint8_t r;              /* colour — 0..255 at full brightness         */
    uint8_t g;
    uint8_t b;
    uint8_t mode;           /* 1=solid 2=blink-1Hz 3=pulse-1Hz 4=fast-4Hz */
    uint8_t brightness_pct; /* 0..100 global brightness                   */
};

/*
 * Fetch the active LED override. Returns true + fills `*out` when an
 * override is active (mode != 0), false otherwise. `out` may be NULL
 * for a bool-only probe.
 */
bool cmd_sys_get_led_override(struct cmd_sys_led_override *out);

/*
 * UART claim state — queried by cdc_glue's pump path. When the driver
 * asserts SYS_UART_CLAIM for a given instance, cdc_glue stops
 * forwarding bytes between the CDC and the UART hardware; reads on
 * the CDC side go silent, writes are drained. Released via
 * SYS_UART_RELEASE. The claim is per UART instance:
 *   0 — UART0 HW (shared by RS232/RS485/TTL CDCs via phy-bank)
 *   1 — UART2 PIO (CDC #3)
 * Values ≥ 2 always return false.
 */
bool cmd_sys_uart_claimed(uint8_t uart_idx);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_SYS_H */
