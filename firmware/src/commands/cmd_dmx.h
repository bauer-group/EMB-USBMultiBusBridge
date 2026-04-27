/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * DMX subsystem dispatcher (subsys = 8) — DMX512-A transmit on a PIO
 * state machine. See docs/protocol/subsystems/dmx.md for the wire
 * format.
 */

#ifndef RPBRIDGE_CMD_DMX_H
#define RPBRIDGE_CMD_DMX_H

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rpbp_status cmd_dmx_dispatch(const struct rpbp_header *hdr,
                                  uint8_t opcode,
                                  const uint8_t *args,
                                  size_t args_len,
                                  bool cbor);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_DMX_H */
