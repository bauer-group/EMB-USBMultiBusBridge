/*
 * SPDX-License-Identifier: MIT
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

/* 2 MB internal flash (stacked) — this overrides the pico-sdk default
 * (rp2350-arm-s.cmake declares 4 MB as the platform default).
 *
 * The C macro and the CMake-side default MUST agree: the C macro feeds
 * `hardware_flash`'s sanity checks at runtime, while the CMake default
 * is interpolated into `pico_flash_region.ld` to bound the linker's
 * FLASH region. If only the C macro is set, `ld` will silently accept
 * binaries up to 4 MB, hiding overflow until flash time.
 *
 * `pico_board_cmake_set_default` is a magic stub: pico.h #defines it
 * to nothing for the C/asm preprocessor, while generic_board.cmake
 * scans the header line-by-line and lifts the value into the CMake
 * scope. See firmware/third_party/pico-sdk/cmake/generic_board.cmake. */
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
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
#ifndef RPBRIDGE_HAVE_RGB_LED
#define RPBRIDGE_HAVE_RGB_LED 1
#endif
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

/* ───── Pin-broker capacity ──────────────────────────────────────────── */

/* RP2354B (QFN-80) exposes GP0..GP47. The pin broker tracks one
 * ownership slot per GPIO; matches the chip's full pin count. */
#ifndef RPBRIDGE_PIN_BROKER_SLOTS
#define RPBRIDGE_PIN_BROKER_SLOTS 48u
#endif

/* ───── UART0 phy-bank (RS232 / RS485 / TTL on a single HW UART) ────── */

#ifndef RPBRIDGE_UART0_TX_PIN
#define RPBRIDGE_UART0_TX_PIN  0u
#endif
#ifndef RPBRIDGE_UART0_RX_PIN
#define RPBRIDGE_UART0_RX_PIN  1u
#endif

/* ───── PIO-UART (4th CDC, TTL only) ─────────────────────────────────── */

#ifndef RPBRIDGE_HAVE_PIO_UART
#define RPBRIDGE_HAVE_PIO_UART  1
#endif
#ifndef RPBRIDGE_PIO_UART_TX_PIN
#define RPBRIDGE_PIO_UART_TX_PIN  33u
#endif
#ifndef RPBRIDGE_PIO_UART_RX_PIN
#define RPBRIDGE_PIO_UART_RX_PIN  36u
#endif

/* ───── I²C — bus 0 (Qwiic 1+2 + 4-pin header) ───────────────────────── */

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

/* ───── I²C — bus 1 (Qwiic 3 + 4-pin header) ─────────────────────────── *
 * Only present on RP2350B (QFN-80) — the SDA/SCL pins live above GP30
 * which the QFN-60 RP2350A package does not bring out. */

#ifndef RPBRIDGE_HAVE_I2C1
#define RPBRIDGE_HAVE_I2C1  1
#endif
#ifndef RPBRIDGE_I2C1_SDA_PIN
#define RPBRIDGE_I2C1_SDA_PIN  34u
#endif
#ifndef RPBRIDGE_I2C1_SCL_PIN
#define RPBRIDGE_I2C1_SCL_PIN  35u
#endif

/* ───── SPI — bus 0 (PMOD header) ────────────────────────────────────── */

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

/* ───── SPI — bus 1 (Breakout header, 3 chip selects) ────────────────── */

#ifndef RPBRIDGE_HAVE_SPI1
#define RPBRIDGE_HAVE_SPI1  1
#endif
#ifndef RPBRIDGE_SPI1_SCK_PIN
#define RPBRIDGE_SPI1_SCK_PIN  10u
#endif
#ifndef RPBRIDGE_SPI1_MOSI_PIN
#define RPBRIDGE_SPI1_MOSI_PIN 11u
#endif
#ifndef RPBRIDGE_SPI1_MISO_PIN
#define RPBRIDGE_SPI1_MISO_PIN 12u
#endif

/* ───── CAN (can2040 on PIO0, GP20/21) ───────────────────────────────── */

#ifndef RPBRIDGE_HAVE_CAN
#define RPBRIDGE_HAVE_CAN  1
#endif
#ifndef RPBRIDGE_CAN_TX_PIN
#define RPBRIDGE_CAN_TX_PIN 20
#endif
#ifndef RPBRIDGE_CAN_RX_PIN
#define RPBRIDGE_CAN_RX_PIN 21
#endif

/* ───── UART phy bank (RS232 + RS485 transceivers) ───────────────────── *
 * Only assembled on the rev-B PCB; a bare Pico 2 board has neither
 * MAX3232 nor SN65HVD72 populated. Disabling these flags collapses the
 * 4×CDC composite to a single TTL CDC over UART0. */

#ifndef RPBRIDGE_HAVE_RS232_PHY
#define RPBRIDGE_HAVE_RS232_PHY  1
#endif
#ifndef RPBRIDGE_HAVE_RS485_PHY
#define RPBRIDGE_HAVE_RS485_PHY  1
#endif

/* Base CDC-ACM count exposed via USB. Computed from the HAVE_*
 * flags so dropping a transceiver bank automatically drops its CDC
 * from the descriptor (and TUSB renumbers the survivors). The TTL
 * UART0 CDC is always present where UART0 GPIOs are wired out;
 * RPBRIDGE_DEBUG_CDC adds one more on top. */
#define RPBRIDGE_BASE_CDC_COUNT  ( \
        (RPBRIDGE_HAVE_RS232_PHY ? 1 : 0) + \
        (RPBRIDGE_HAVE_RS485_PHY ? 1 : 0) + \
        1 /* UART0 TTL */ + \
        (RPBRIDGE_HAVE_PIO_UART  ? 1 : 0))
#ifndef RPBRIDGE_RS485_DE_PIN
#define RPBRIDGE_RS485_DE_PIN  6
#endif
#ifndef RPBRIDGE_RS485_EN_PIN
#define RPBRIDGE_RS485_EN_PIN  7
#endif
#ifndef RPBRIDGE_RS232_SHDN_PIN
#define RPBRIDGE_RS232_SHDN_PIN  8
#endif

/* ───── WS2812 / addressable LED strip (PIO2) ───────────────────────── *
 * One pin drives the strip's data line directly (idle LOW, NRZ-encoded).
 * The pin is part of the user-GPIO whitelist; the broker prevents
 * overlapping GPIO/PWM claims at runtime. */

#ifndef RPBRIDGE_HAVE_LED
#define RPBRIDGE_HAVE_LED  1
#endif
#ifndef RPBRIDGE_LED_STRIP_PIN
#define RPBRIDGE_LED_STRIP_PIN  43u
#endif

/* ───── DMX512 (PIO2) ────────────────────────────────────────────────── *
 * Single TX pin — the user wires an external RS-485 transceiver
 * (MAX485 / SN75176). Tx-only in v1; RX is a future extension. */

#ifndef RPBRIDGE_HAVE_DMX
#define RPBRIDGE_HAVE_DMX  1
#endif
#ifndef RPBRIDGE_DMX_TX_PIN
#define RPBRIDGE_DMX_TX_PIN  44u
#endif

/* ───── 1-Wire (PIO1, shared block with the PIO-UART) ───────────────── *
 * Single bidirectional pin with an external 4.7 kΩ pull-up to +3V3.
 * The PIO program emulates open-drain by toggling pindirs (drive LOW
 * vs. high-Z + pull-up = HIGH). */

#ifndef RPBRIDGE_HAVE_ONEWIRE
#define RPBRIDGE_HAVE_ONEWIRE  1
#endif
#ifndef RPBRIDGE_ONEWIRE_PIN
#define RPBRIDGE_ONEWIRE_PIN  32u
#endif

/* ───── I²S TX (PIO2) ────────────────────────────────────────────────── *
 * Three pins: BCLK (bit clock), LRCLK / WS (word select / left-right),
 * DOUT (serial data). BCLK and LRCLK are driven by side-set on
 * consecutive pins (BCLK at base, LRCLK = base + 1). DOUT is the OUT
 * pin and may be located independently. */

#ifndef RPBRIDGE_HAVE_I2S
#define RPBRIDGE_HAVE_I2S  1
#endif
#ifndef RPBRIDGE_I2S_BCLK_PIN
#define RPBRIDGE_I2S_BCLK_PIN  41u
#endif
#ifndef RPBRIDGE_I2S_LRCLK_PIN
#define RPBRIDGE_I2S_LRCLK_PIN  42u   /* MUST be RPBRIDGE_I2S_BCLK_PIN + 1 */
#endif
#ifndef RPBRIDGE_I2S_DOUT_PIN
#define RPBRIDGE_I2S_DOUT_PIN  45u
#endif

/* ───── ADC channel map ──────────────────────────────────────────────── *
 * Same on RP2350A and RP2350B — channels 0..3 share GP26..GP29 in both
 * packages, channel 4 is the on-die temperature sensor. */

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
 * The board exposes these GPIO numbers on its 2×10 user header. The
 * lists are macros so source files can iterate over them with a fixed
 * static const array. The PWM list is a strict subset of the GPIO list
 * (only PWM-capable slices land in the PWM whitelist). */

#ifndef RPBRIDGE_USER_GPIO_PIN_COUNT
#define RPBRIDGE_USER_GPIO_PIN_COUNT  11u
#endif
#ifndef RPBRIDGE_USER_GPIO_PIN_LIST
#define RPBRIDGE_USER_GPIO_PIN_LIST   { 30u, 31u, 37u, 38u, 39u, 40u, \
                                        41u, 42u, 43u, 44u, 45u }
#endif

#ifndef RPBRIDGE_USER_PWM_PIN_COUNT
#define RPBRIDGE_USER_PWM_PIN_COUNT  6u
#endif
#ifndef RPBRIDGE_USER_PWM_PIN_LIST
#define RPBRIDGE_USER_PWM_PIN_LIST   { 30u, 31u, 37u, 38u, 39u, 40u }
#endif

#include "boards/pico2.h"   /* base defaults from the SDK */

#endif /* _BOARDS_RPBRIDGE_RP2354B_H */
