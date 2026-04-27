/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * I²C subsystem handlers (subsys = 1). Binary opcodes mapped to the
 * i2c_hw wrapper; the spec lives in docs/protocol/subsystems/i2c.md.
 */

#ifndef RPBRIDGE_CMD_I2C_H
#define RPBRIDGE_CMD_I2C_H

#include "rpbp/rpbp.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rpbp_status cmd_i2c_dispatch(const struct rpbp_header *req,
                                  uint8_t opcode,
                                  const uint8_t *args, size_t args_len,
                                  bool cbor);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_I2C_H */
