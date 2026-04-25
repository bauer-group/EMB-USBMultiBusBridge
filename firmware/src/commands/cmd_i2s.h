/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * I²S subsystem dispatcher (subsys = 10). See
 * docs/protocol/subsystems/i2s.md.
 */

#ifndef RPBRIDGE_CMD_I2S_H
#define RPBRIDGE_CMD_I2S_H

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rpbp_status cmd_i2s_dispatch(const struct rpbp_header *hdr,
                                  uint8_t opcode,
                                  const uint8_t *args,
                                  size_t args_len,
                                  bool cbor);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_I2S_H */
