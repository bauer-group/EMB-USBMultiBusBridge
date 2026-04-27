/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * cdc_glue — bridges USB CDC-ACM interfaces to the UART peripherals.
 *
 * Mapping:
 *   CDC-ACM #0 <-> UART0 HW (phy-bank on GP0/GP1)
 *   CDC-ACM #1 <-> PIO-UART #2    (pending — lands with M4b)
 *
 * Forwarded out-of-band signals (TinyUSB callbacks):
 *   - Line coding (baud / data / stop / parity) -> uart_hw_set_line_coding
 *   - Send-break                                -> uart_set_break
 *   - Line-state DTR/RTS                        -> logged (Arduino-style
 *                                                 reset trick pending)
 */

#ifndef RPBRIDGE_CDC_GLUE_H
#define RPBRIDGE_CDC_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One-time setup: initialises UART0 on the phy-bank pins and arms
 * everything the TinyUSB callbacks need. Call once after tusb_init()
 * and before cdc_glue_poll().
 */
void cdc_glue_init(void);

/*
 * Non-blocking pump. Call from the main loop after tud_task(). Moves
 * bytes USB → UART and UART → USB for every wired CDC interface,
 * honouring both the UART TX ring capacity and the CDC write-buffer
 * capacity so no bytes are silently dropped.
 */
void cdc_glue_poll(void);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_CDC_GLUE_H */
