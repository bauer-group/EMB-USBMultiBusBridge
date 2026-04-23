/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * TinyUSB configuration for the RPBridge composite device.
 *
 * The composite consists of:
 *   - 3× CDC-ACM (user UART #1, user UART #2, debug log)
 *   - 1× Vendor interface bound via MS OS 2.0 -> WinUSB on Windows,
 *        used as the RPBP transport (bulk IN/OUT, 64 B FS packets).
 *   - 1× Vendor interface speaking gs_usb for CAN (mainline Linux driver
 *        binds automatically, Windows tools via candle_api).
 */

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ───── Board / MCU ──────────────────────────────────────────────────── */
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU              OPT_MCU_RP2040   /* RP2350 shares the RP2040 TinyUSB driver */
#endif
#define CFG_TUSB_OS               OPT_OS_PICO
#define CFG_TUSB_DEBUG            0

/* ───── Device class capacities ──────────────────────────────────────── */

#define CFG_TUD_ENABLED           1
#define CFG_TUD_ENDPOINT0_SIZE    64

/*
 * Two CDC-ACMs = two user virtual COM ports:
 *   index 0 -> UART 1 (UART0 HW phy-bank: RS232 / RS485 / TTL)
 *   index 1 -> UART 2 (PIO-UART #2 on GP33/GP36)
 *
 * We deliberately do NOT expose a USB debug-log CDC — stdio is routed
 * to UART1 on the 5-pin debug header instead (see CMakeLists.txt).
 */
#define CFG_TUD_CDC               2
#define CFG_TUD_VENDOR            2   /* RPBP transport + gs_usb */
#define CFG_TUD_HID               0
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_AUDIO             0
#define CFG_TUD_NET               0

/* ───── CDC FIFO sizes ───────────────────────────────────────────────── */

#define CFG_TUD_CDC_RX_BUFSIZE    256
#define CFG_TUD_CDC_TX_BUFSIZE    256
#define CFG_TUD_CDC_EP_BUFSIZE    64

/* ───── Vendor-interface FIFO sizes (RPBP + gs_usb) ──────────────────── */

#define CFG_TUD_VENDOR_RX_BUFSIZE 4096
#define CFG_TUD_VENDOR_TX_BUFSIZE 4096
#define CFG_TUD_VENDOR_EPSIZE     64

/* ───── Memory placement ─────────────────────────────────────────────── */

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))
#endif

#ifdef __cplusplus
}
#endif
#endif /* TUSB_CONFIG_H */
