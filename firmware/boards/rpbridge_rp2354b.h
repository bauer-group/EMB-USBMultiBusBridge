/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Custom board header for the RPBridge rev-B hardware: RP2354B (RP2350B
 * die + 2 MB stacked QSPI flash in QFN-80). Pin map follows
 * docs/drafts/rpbridge-hardware-design.md §6.
 *
 * This file is selected by CMake via -DPICO_BOARD=rpbridge_rp2354b.
 * It derives from the pico-sdk's pico2_w board header, stripped of
 * features we do not have (WiFi chip) and extended with our own
 * peripheral defaults.
 */

#ifndef _BOARDS_RPBRIDGE_RP2354B_H
#define _BOARDS_RPBRIDGE_RP2354B_H

/* ───── Platform ─────────────────────────────────────────────────────── */

#ifndef PICO_RP2350
#define PICO_RP2350 1
#endif

/* RP2354B is the RP2350B die plus 2 MB stacked QSPI flash. */
#ifndef PICO_RP2350B
#define PICO_RP2350B 1
#endif

/* 2 MB internal flash (stacked) — this overrides the pico-sdk default. */
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
#endif

/* ───── Clock & regulator ────────────────────────────────────────────── */

#ifndef PICO_BOOT_STAGE2_CHOOSE_W25Q080
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#endif

#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif

/* ───── USB ──────────────────────────────────────────────────────────── */

/* No dedicated activity LED on the USB stack — our RGB LED is firmware-
 * animated so the bootloader doesn't fight us for GPIOs. */
#ifndef PICO_DEFAULT_USB_ACTIVITY_PIN
#define PICO_DEFAULT_USB_ACTIVITY_PIN -1
#endif

/* ───── On-board amenities ───────────────────────────────────────────── */

/* Single discrete RGB LED on GP22/GP23/GP24 — common-anode, GPIO sinks. */
#ifndef RPBRIDGE_LED_R_PIN
#define RPBRIDGE_LED_R_PIN 22
#endif
#ifndef RPBRIDGE_LED_G_PIN
#define RPBRIDGE_LED_G_PIN 23
#endif
#ifndef RPBRIDGE_LED_B_PIN
#define RPBRIDGE_LED_B_PIN 24
#endif

/* User button — GP25 (front-panel push button). */
#ifndef RPBRIDGE_USER_BUTTON_PIN
#define RPBRIDGE_USER_BUTTON_PIN 25
#endif

/* VBUS sense via resistor divider into GP46. */
#ifndef PICO_VBUS_PIN
#define PICO_VBUS_PIN 46
#endif

/* ───── UART defaults ────────────────────────────────────────────────── */

/* UART1 (GP4/GP5) is the debug log routed to the 5-pin debug header. */
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 1
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 4
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 5
#endif

/* ───── I²C defaults (bus 0 via Qwiic headers) ───────────────────────── */

#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 2
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 3
#endif

/* ───── SPI defaults (bus 0 via PMOD) ────────────────────────────────── */

#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 18
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 19
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 16
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 17
#endif

/* ───── CAN (PIO0 dedicated) ─────────────────────────────────────────── */

#ifndef RPBRIDGE_CAN_TX_PIN
#define RPBRIDGE_CAN_TX_PIN 20
#endif
#ifndef RPBRIDGE_CAN_RX_PIN
#define RPBRIDGE_CAN_RX_PIN 21
#endif

/* ───── UART phy selection lines (RS232/RS485/TTL) ───────────────────── */

#ifndef RPBRIDGE_RS485_DE_PIN
#define RPBRIDGE_RS485_DE_PIN  6
#endif
#ifndef RPBRIDGE_RS485_EN_PIN
#define RPBRIDGE_RS485_EN_PIN  7
#endif
#ifndef RPBRIDGE_RS232_SHDN_PIN
#define RPBRIDGE_RS232_SHDN_PIN  8
#endif

#include "boards/pico2.h"   /* base defaults from the SDK */

#endif /* _BOARDS_RPBRIDGE_RP2354B_H */
