/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * spi — thin wrapper over the RP2350 hardware_spi (PL022) for both
 * SPI instances.
 *
 * Pin map (rev-B, see boards/rpbridge_rp2354b.h):
 *   SPI #0 (PMOD)     : SCK=GP18, MOSI=GP19, MISO=GP16, CS=GP17
 *   SPI #1 (Breakout) : SCK=GP10, MOSI=GP11, MISO=GP12, CS0=GP13,
 *                                                      CS1=GP14,
 *                                                      CS2=GP15
 *
 * Chip-select lines are driven **manually** by this driver as plain
 * GPIOs, not via the PL022 frame-slave-select output. Manual CS is:
 *   - simpler (no spurious CS toggles between FIFO bursts);
 *   - necessary to support multi-CS banks on SPI1;
 *   - and matches the RPBP wire format which passes a raw CS pin.
 *
 * Supported line rates: ~3.8 kHz .. 62.5 MHz (clkdiv bounds around
 * clk_peri at 125 MHz). The driver clamps user requests into that
 * window and reports the actual applied rate back via spi_hw_freq().
 */

#ifndef RPBRIDGE_SPI_H
#define RPBRIDGE_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t spi_hw_instance_t;

/* SPI #1 (the breakout-header bus with 3 chip selects) is conditional
 * on the board: rev-B (RP2350B QFN-80) ships both buses; Pi Pico 2
 * (RP2350A QFN-60) does not bring out the SPI #1 pins on the user
 * header layout we ship and so collapses to one bus. */
#include "pico.h"
#ifndef RPBRIDGE_HAVE_SPI1
#  define RPBRIDGE_HAVE_SPI1 1
#endif
#if RPBRIDGE_HAVE_SPI1
#  define SPI_HW_NUM_INSTANCES  2u
#else
#  define SPI_HW_NUM_INSTANCES  1u
#endif

/* Mode bits — match RPBP wire values (rpbp.h RPBP_SPI_MODE_*). */
#define SPI_HW_MODE_CPHA        (1u << 0)
#define SPI_HW_MODE_CPOL        (1u << 1)
#define SPI_HW_MODE_LSB_FIRST   (1u << 2)

/*
 * Initialise every SPI instance with default line parameters
 * (1 MHz, mode 0, MSB-first) and claim its SCK/MOSI/MISO pins on the
 * pin broker. CS pins are NOT claimed here — they are grabbed on the
 * first XFER so the user can run a bare MOSI-loopback without holding
 * CS lines hostage.
 *
 * Returns 0 on success, -RPBP_EBUSY on pin conflict.
 */
int spi_hw_init_all(void);

/*
 * Set the SPI line rate (Hz). Returns the actual applied rate (the
 * PL022 only hits discrete divisors; the hardware_spi SDK helper
 * rounds downward). Returns 0 on error.
 */
uint32_t spi_hw_set_freq(spi_hw_instance_t instance, uint32_t freq_hz);

uint32_t spi_hw_freq(spi_hw_instance_t instance);

/*
 * Set the SPI mode. `mode_bits` is a bitmask of SPI_HW_MODE_*.
 * LSB_FIRST is rejected with -RPBP_ENOTSUP (PL022 is MSB-only).
 * Returns 0 on success.
 */
int spi_hw_set_mode(spi_hw_instance_t instance, uint8_t mode_bits);

uint8_t spi_hw_mode(spi_hw_instance_t instance);

/*
 * Acquire a CS pin as an output and drive it HIGH (deasserted).
 * Claims the pin on the broker under PIN_OWNER_SPI the first time
 * it is seen. Same-owner re-claims are idempotent. Returns 0 on
 * success, -RPBP_EBUSY on conflict.
 */
int spi_hw_cs_acquire(uint8_t cs_pin);

/* Drive the given CS pin LOW (asserted) / HIGH (released). Both are
 * no-ops if `cs_pin` == 0xFF. Returns -RPBP_EINVAL on unclaimed pin. */
int spi_hw_cs_assert (uint8_t cs_pin);
int spi_hw_cs_release(uint8_t cs_pin);

/*
 * Full-duplex transfer. `tx` MAY be NULL (writes 0x00 padding).
 * `rx` MAY be NULL (discards read data). `len` is the SPI frame
 * count (1 byte per frame; 16-bit transfers are not exposed over
 * the RPBP wire format).
 *
 * CS semantics:
 *   - `cs_pin == 0xFF` : no CS manipulation (the caller handles it
 *                        via spi_hw_cs_assert / spi_hw_cs_release).
 *   - otherwise        : drives CS low before the transfer and, if
 *                        `hold_cs == false`, high after.
 *
 * Returns the number of bytes transferred on success, or a negative
 * RPBP status code (-RPBP_EINVAL / -RPBP_EIO / -RPBP_EBUSY).
 */
int spi_hw_xfer(spi_hw_instance_t instance,
                uint8_t cs_pin, bool hold_cs,
                const uint8_t *tx, uint8_t *rx, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_SPI_H */
