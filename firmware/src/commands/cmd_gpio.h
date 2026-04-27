/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * GPIO subsystem dispatcher (subsys = 3). Opcodes documented in
 * docs/protocol/subsystems/gpio.md.
 */

#ifndef RPBRIDGE_CMD_GPIO_H
#define RPBRIDGE_CMD_GPIO_H

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rpbp_status cmd_gpio_dispatch(const struct rpbp_header *hdr,
                                   uint8_t opcode,
                                   const uint8_t *args,
                                   size_t args_len,
                                   bool cbor);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_GPIO_H */
