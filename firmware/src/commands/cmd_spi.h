/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * SPI subsystem dispatcher (subsys = 2). Wire format specified in
 * docs/protocol/subsystems/spi.md.
 */

#ifndef RPBRIDGE_CMD_SPI_H
#define RPBRIDGE_CMD_SPI_H

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rpbp_status cmd_spi_dispatch(const struct rpbp_header *hdr,
                                  uint8_t opcode,
                                  const uint8_t *args,
                                  size_t args_len,
                                  bool cbor);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_SPI_H */
