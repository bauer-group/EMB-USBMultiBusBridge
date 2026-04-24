/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * ADC subsystem dispatcher (subsys = 5). Wire format in
 * docs/protocol/subsystems/adc.md.
 */

#ifndef RPBRIDGE_CMD_ADC_H
#define RPBRIDGE_CMD_ADC_H

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rpbp_status cmd_adc_dispatch(const struct rpbp_header *hdr,
                                  uint8_t opcode,
                                  const uint8_t *args,
                                  size_t args_len,
                                  bool cbor);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CMD_ADC_H */
