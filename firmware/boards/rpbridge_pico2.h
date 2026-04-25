/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * Custom board header for the Raspberry Pi Pico 2 — RP2350A in the
 * QFN-60 package, with an external 4 MB W25Q32JV QSPI flash. Same
 * silicon as the RP2354B (rev-B board) but only GP0..GP29 are pinned
 * out, so a few subsystems collapse:
 *
 *   - I²C #1 (was GP34/35 on rev-B)         → not available
 *   - SPI #1 breakout (was GP10..15)        → not available
 *   - PIO-UART (was GP33/36 on rev-B)       → remapped to GP6/GP7
 *   - RS232 / RS485 transceivers            → not populated
 *
 * The other subsystems map identically. Capability map advertises
 * only what's present so the Linux driver spawns the right
 * aux_devices automatically.
 *
 * Selected via CMake `-DPICO_BOARD=rpbridge_pico2`.
 */

#ifndef _BOARDS_RPBRIDGE_PICO2_H
#define _BOARDS_RPBRIDGE_PICO2_H

/* ───── Platform ─────────────────────────────────────────────────────── */

#ifndef PICO_RP2350
#define PICO_RP2350 1
#endif

/* RP2350A is the QFN-60 die — same architecture as RP2350B but with
 * fewer pins exposed. */
#ifndef PICO_RP2350A
#define PICO_RP2350A 1
#endif

/* External 4 MB QSPI flash on the Pi Pico 2 reference design
 * (W25Q32JV). The pico-sdk default boot stage 2 covers this part. */
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4u * 1024u * 1024u)
#endif

/* ───── Board identification ─────────────────────────────────────────── */

#ifndef RPBRIDGE_BOARD_NAME
#define RPBRIDGE_BOARD_NAME "rpbridge-pico2"
#endif

/* ───── Pin-broker capacity ──────────────────────────────────────────── */

/* RP2350A (QFN-60) exposes GP0..GP29 → 30 broker slots. */
#ifndef RPBRIDGE_PIN_BROKER_SLOTS
#define RPBRIDGE_PIN_BROKER_SLOTS 30u
#endif

/* ───── On-board amenities ───────────────────────────────────────────── *
 *
 * The Pi Pico 2 board carries only a single green LED on GP25; the
 * RPBridge firmware drives an RGB LED via PWM. We collapse R/G/B
 * onto GP25 so any colour request lights the single LED. A proper
 * single-LED mode (no PWM, no R/G/B mixing) is a future cleanup.
 */
#ifndef RPBRIDGE_LED_R_PIN
#define RPBRIDGE_LED_R_PIN 25
#endif
#ifndef RPBRIDGE_LED_G_PIN
#define RPBRIDGE_LED_G_PIN 25
#endif
#ifndef RPBRIDGE_LED_B_PIN
#define RPBRIDGE_LED_B_PIN 25
#endif

/* No user button on the bare Pico 2 — leave the firmware's hook
 * pointing at an unused pin. Anyone who wires a button can override
 * this in their own variant header. */
#ifndef RPBRIDGE_USER_BUTTON_PIN
#define RPBRIDGE_USER_BUTTON_PIN 28
#endif

/* VBUS sense via VSYS divider on GP24 (Pi Pico convention). */
#ifndef PICO_VBUS_PIN
#define PICO_VBUS_PIN 24
#endif

/* ───── UART defaults ────────────────────────────────────────────────── *
 *
 * Debug stdio on UART1 (GP4/GP5) — same as rev-B.
 */
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 1
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 4
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 5
#endif

/* ───── UART0 (the user-facing CDC-bridged HW UART) ─────────────────── */

#ifndef RPBRIDGE_UART0_TX_PIN
#define RPBRIDGE_UART0_TX_PIN  0u
#endif
#ifndef RPBRIDGE_UART0_RX_PIN
#define RPBRIDGE_UART0_RX_PIN  1u
#endif

/* ───── PIO-UART (4th CDC, TTL) — remapped to GP6/GP7 ───────────────── */

#ifndef RPBRIDGE_HAVE_PIO_UART
#define RPBRIDGE_HAVE_PIO_UART  1
#endif
#ifndef RPBRIDGE_PIO_UART_TX_PIN
#define RPBRIDGE_PIO_UART_TX_PIN  6u
#endif
#ifndef RPBRIDGE_PIO_UART_RX_PIN
#define RPBRIDGE_PIO_UART_RX_PIN  7u
#endif

/* ───── I²C — bus 0 only (no QFN-60 pad for bus 1) ──────────────────── */

#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 2
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 3
#endif
#ifndef RPBRIDGE_I2C0_SDA_PIN
#define RPBRIDGE_I2C0_SDA_PIN  PICO_DEFAULT_I2C_SDA_PIN
#endif
#ifndef RPBRIDGE_I2C0_SCL_PIN
#define RPBRIDGE_I2C0_SCL_PIN  PICO_DEFAULT_I2C_SCL_PIN
#endif

#ifndef RPBRIDGE_HAVE_I2C1
#define RPBRIDGE_HAVE_I2C1  0
#endif

/* ───── SPI — bus 0 only ─────────────────────────────────────────────── */

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
#ifndef RPBRIDGE_SPI0_SCK_PIN
#define RPBRIDGE_SPI0_SCK_PIN  PICO_DEFAULT_SPI_SCK_PIN
#endif
#ifndef RPBRIDGE_SPI0_MOSI_PIN
#define RPBRIDGE_SPI0_MOSI_PIN PICO_DEFAULT_SPI_TX_PIN
#endif
#ifndef RPBRIDGE_SPI0_MISO_PIN
#define RPBRIDGE_SPI0_MISO_PIN PICO_DEFAULT_SPI_RX_PIN
#endif

#ifndef RPBRIDGE_HAVE_SPI1
#define RPBRIDGE_HAVE_SPI1  0
#endif

/* ───── CAN (can2040 on PIO0, GP20/21 — same as rev-B) ─────────────── */

#ifndef RPBRIDGE_HAVE_CAN
#define RPBRIDGE_HAVE_CAN  1
#endif
#ifndef RPBRIDGE_CAN_TX_PIN
#define RPBRIDGE_CAN_TX_PIN 20
#endif
#ifndef RPBRIDGE_CAN_RX_PIN
#define RPBRIDGE_CAN_RX_PIN 21
#endif

/* ───── UART transceivers — NOT populated on bare Pi Pico 2 ─────────── */

#ifndef RPBRIDGE_HAVE_RS232_PHY
#define RPBRIDGE_HAVE_RS232_PHY  0
#endif
#ifndef RPBRIDGE_HAVE_RS485_PHY
#define RPBRIDGE_HAVE_RS485_PHY  0
#endif
/* The RS232/RS485 control pins are inert on this variant but kept
 * defined so the build doesn't trip on a missing identifier. The
 * #if RPBRIDGE_HAVE_*_PHY guards in uart_phy.c keep them unused. */
#ifndef RPBRIDGE_RS485_DE_PIN
#define RPBRIDGE_RS485_DE_PIN  255u
#endif
#ifndef RPBRIDGE_RS485_EN_PIN
#define RPBRIDGE_RS485_EN_PIN  255u
#endif
#ifndef RPBRIDGE_RS232_SHDN_PIN
#define RPBRIDGE_RS232_SHDN_PIN  255u
#endif

/* ───── WS2812 / addressable LED strip (PIO2) ───────────────────────── *
 * Drives a single addressable strip directly from a 3.3 V GPIO. Most
 * WS2812-clone strips accept 3.3 V data with a level-shifter, or the
 * first pixel can be sacrificed as a passive level shifter.
 *
 * Pin overlaps with the user-GPIO/PWM whitelist; the pin broker
 * surfaces a runtime conflict if the host tries to GPIO/PWM the same
 * pad while the LED subsystem is bound. */

#ifndef RPBRIDGE_HAVE_LED
#define RPBRIDGE_HAVE_LED  1
#endif
#ifndef RPBRIDGE_LED_STRIP_PIN
#define RPBRIDGE_LED_STRIP_PIN  14u
#endif

/* ───── DMX512 (PIO2) ────────────────────────────────────────────────── *
 * Single TX pin — user wires an external RS-485 transceiver. Tx-only
 * in v1. */

#ifndef RPBRIDGE_HAVE_DMX
#define RPBRIDGE_HAVE_DMX  1
#endif
#ifndef RPBRIDGE_DMX_TX_PIN
#define RPBRIDGE_DMX_TX_PIN  15u
#endif

/* ───── 1-Wire (PIO1) ────────────────────────────────────────────────── *
 * One pin with external 4.7 kΩ pull-up. Pin overlaps the user-GPIO
 * whitelist; the broker arbitrates at runtime. */

#ifndef RPBRIDGE_HAVE_ONEWIRE
#define RPBRIDGE_HAVE_ONEWIRE  1
#endif
#ifndef RPBRIDGE_ONEWIRE_PIN
#define RPBRIDGE_ONEWIRE_PIN  12u
#endif

/* ───── I²S TX (PIO2) ────────────────────────────────────────────────── *
 * BCLK + LRCLK consecutive (LRCLK MUST be BCLK+1). DOUT independent. */

#ifndef RPBRIDGE_HAVE_I2S
#define RPBRIDGE_HAVE_I2S  1
#endif
#ifndef RPBRIDGE_I2S_BCLK_PIN
#define RPBRIDGE_I2S_BCLK_PIN  8u
#endif
#ifndef RPBRIDGE_I2S_LRCLK_PIN
#define RPBRIDGE_I2S_LRCLK_PIN  9u
#endif
#ifndef RPBRIDGE_I2S_DOUT_PIN
#define RPBRIDGE_I2S_DOUT_PIN  13u
#endif

/* ───── ADC channel map — identical to rev-B ────────────────────────── */

#ifndef RPBRIDGE_ADC_EXT_CHANNEL_COUNT
#define RPBRIDGE_ADC_EXT_CHANNEL_COUNT  4u
#endif
#ifndef RPBRIDGE_ADC_CH0_PIN
#define RPBRIDGE_ADC_CH0_PIN  26u
#endif
#ifndef RPBRIDGE_ADC_CH1_PIN
#define RPBRIDGE_ADC_CH1_PIN  27u
#endif
#ifndef RPBRIDGE_ADC_CH2_PIN
#define RPBRIDGE_ADC_CH2_PIN  28u
#endif
#ifndef RPBRIDGE_ADC_CH3_PIN
#define RPBRIDGE_ADC_CH3_PIN  29u
#endif

/* ───── User-facing GPIO + PWM whitelists ────────────────────────────── *
 *
 * GP8..GP15 are free user pins on the Pi Pico 2 (after carving out
 * UART0, I²C #0, debug UART, PIO-UART, SPI #0, CAN, ADC). All eight
 * are PWM-capable on RP2350A (slices 4-7, channels A/B), but we
 * publish only six in the PWM whitelist to leave room for two pure-
 * GPIO pins on the user header.
 */

#ifndef RPBRIDGE_USER_GPIO_PIN_COUNT
#define RPBRIDGE_USER_GPIO_PIN_COUNT  8u
#endif
#ifndef RPBRIDGE_USER_GPIO_PIN_LIST
#define RPBRIDGE_USER_GPIO_PIN_LIST   { 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u }
#endif

#ifndef RPBRIDGE_USER_PWM_PIN_COUNT
#define RPBRIDGE_USER_PWM_PIN_COUNT  6u
#endif
#ifndef RPBRIDGE_USER_PWM_PIN_LIST
#define RPBRIDGE_USER_PWM_PIN_LIST   { 8u, 9u, 10u, 11u, 12u, 13u }
#endif

#include "boards/pico2.h"   /* base defaults from the SDK */

#endif /* _BOARDS_RPBRIDGE_PICO2_H */
