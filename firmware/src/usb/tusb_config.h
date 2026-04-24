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

/* Root-hub port 0 is the only USB port on RP2040/RP2350 and runs in
 * device mode for this firmware. Newer TinyUSB (≥ 0.18) requires the
 * mode macros to be present at compile time for `tusb_init()`'s
 * static assertions. */
#define CFG_TUSB_RHPORT0_MODE     OPT_MODE_DEVICE
#define CFG_TUSB_RHPORT1_MODE     OPT_MODE_NONE

/* ───── Device class capacities ──────────────────────────────────────── */

#define CFG_TUD_ENABLED           1
#define CFG_TUD_ENDPOINT0_SIZE    64

/*
 * Four CDC-ACMs — one per user-facing phy, so a stock terminal (no
 * driver, no rpbridge-ctl) can reach every physical layer by simply
 * opening the correctly-named virtual COM port:
 *   index 0 -> UART 1 RS232  (MAX3232 on UART0 phy-bank)
 *   index 1 -> UART 1 RS485  (SN65HVD72 on UART0 phy-bank, auto DE)
 *   index 2 -> UART 1 TTL    (direct 3.3 V drive on UART0 phy-bank)
 *   index 3 -> UART 2 TTL    (PIO-UART #2 on GP33/GP36)
 *
 * The firmware watches CDC DTR state and auto-selects the phy-bank
 * transceiver for whichever UART0 CDC is currently active. Only one
 * UART0 CDC may own UART0 HW at a time; others silently drain.
 *
 * We deliberately do NOT expose a USB debug-log CDC — stdio is routed
 * to UART1 on the 5-pin debug header instead (see CMakeLists.txt).
 */
#define CFG_TUD_CDC               4
#define CFG_TUD_VENDOR            2   /* RPBP transport + gs_usb */
#define CFG_TUD_HID               0
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_AUDIO             0
#define CFG_TUD_ECM_RNDIS         0

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
