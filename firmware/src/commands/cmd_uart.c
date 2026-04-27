/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * UART subsystem handlers (subsys = 6).
 *
 * Two opcodes in the v1 surface:
 *   - SET_PHY(uart_idx, phy)   switches the UART0 phy-bank
 *   - GET_PHY(uart_idx)        returns the currently selected phy
 *
 * uart_idx mapping:
 *   0 → UART0 HW (phy-bank capable; SET_PHY is meaningful)
 *   1 → PIO-UART #2 (TTL-only; SET_PHY → ENOTSUP, GET_PHY → TTL)
 */

#include "cmd_uart.h"

#include "rpbp/rpbp.h"
#include "rpbp/rpbp_dispatch.h"
#include "peripherals/uart_phy.h"

#define UART_IDX_HW   0u   /* matches uart_phy / uart_hw instance 0 */
#define UART_IDX_PIO  1u   /* PIO-UART #2, TTL-only                  */

static enum rpbp_status op_set_phy(const struct rpbp_header *req,
                                   const uint8_t *args, size_t args_len)
{
    if (args_len != 2u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_UART, RPBP_UART_SET_PHY,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    const uint8_t uart_idx = args[0];
    const uint8_t phy      = args[1];

    if (uart_idx != UART_IDX_HW) {
        /* PIO-UART or unknown index: phy switching not supported. */
        rpbp_reply_cmd(req, RPBP_SUBSYS_UART, RPBP_UART_SET_PHY,
                       RPBP_ENOTSUP, NULL, 0, false);
        return RPBP_OK;
    }

    if (phy > RPBP_UART_PHY_RS485) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_UART, RPBP_UART_SET_PHY,
                       RPBP_EINVAL, NULL, 0, false);
        return RPBP_OK;
    }

    int rc = uart_phy_set((enum uart_phy_mode)phy);
    uint8_t status = (rc == 0) ? RPBP_OK : RPBP_EINVAL;
    rpbp_reply_cmd(req, RPBP_SUBSYS_UART, RPBP_UART_SET_PHY, status,
                   NULL, 0, false);
    return RPBP_OK;
}

static enum rpbp_status op_get_phy(const struct rpbp_header *req,
                                   const uint8_t *args, size_t args_len)
{
    if (args_len != 1u) {
        rpbp_reply_cmd(req, RPBP_SUBSYS_UART, RPBP_UART_GET_PHY,
                       RPBP_EMSGSIZE, NULL, 0, false);
        return RPBP_OK;
    }

    const uint8_t uart_idx = args[0];
    uint8_t       phy;

    switch (uart_idx) {
    case UART_IDX_HW:
        phy = (uint8_t)uart_phy_get();
        break;
    case UART_IDX_PIO:
        phy = (uint8_t)RPBP_UART_PHY_TTL;
        break;
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_UART, RPBP_UART_GET_PHY,
                       RPBP_ENOENT, NULL, 0, false);
        return RPBP_OK;
    }

    rpbp_reply_cmd(req, RPBP_SUBSYS_UART, RPBP_UART_GET_PHY, RPBP_OK,
                   &phy, 1u, false);
    return RPBP_OK;
}

enum rpbp_status cmd_uart_dispatch(const struct rpbp_header *req,
                                   uint8_t opcode,
                                   const uint8_t *args, size_t args_len,
                                   bool cbor)
{
    (void)cbor;     /* both opcodes take binary args only */

    switch (opcode) {
    case RPBP_UART_SET_PHY: return op_set_phy(req, args, args_len);
    case RPBP_UART_GET_PHY: return op_get_phy(req, args, args_len);
    default:
        rpbp_reply_cmd(req, RPBP_SUBSYS_UART, opcode, RPBP_ENOENT,
                       NULL, 0, false);
        return RPBP_OK;
    }
}
