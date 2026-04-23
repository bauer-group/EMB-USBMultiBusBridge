/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * UART subsystem (subsys = 6). Implements the opcodes documented in
 * docs/protocol/subsystems/uart.md.
 */

#ifndef RPBRIDGE_CMD_UART_H
#define RPBRIDGE_CMD_UART_H

#include "rpbp/rpbp.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rpbp_status cmd_uart_dispatch(const struct rpbp_header *req,
                                   uint8_t opcode,
                                   const uint8_t *args, size_t args_len,
                                   bool cbor);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_UART_H */
