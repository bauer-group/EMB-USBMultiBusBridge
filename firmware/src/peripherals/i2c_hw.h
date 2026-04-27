/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * i2c_hw — thin wrapper around the RP2350 hardware I²C controller.
 *
 * Exposes the two on-chip I²C buses to the rest of the firmware as
 * a small, single-caller API. A transfer writes zero or more bytes,
 * optionally repeated-starts into a read, and terminates with STOP
 * unless `no_stop` is requested.
 *
 * Threading: callers run on Core 0 and serialise through the RPBP
 * command dispatcher. A transfer on bus 0 does not interfere with a
 * transfer on bus 1; within a single bus, transfers are serialised
 * by construction.
 *
 * Pin layout (hardware-design.md §6):
 *   bus 0 → SDA = GP2, SCL = GP3  (Qwiic 1+2 + I²C0 header)
 *   bus 1 → SDA = GP34, SCL = GP35 (Qwiic 3 + I²C1 header)
 */

#ifndef RPBRIDGE_I2C_HW_H
#define RPBRIDGE_I2C_HW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which pico-sdk I²C instance — 0 or 1. */
typedef uint8_t i2c_hw_instance_t;

/* Supported baud rates in Hz. The hardware supports 10 kHz..1 MHz
 * smoothly; advertise only the canonical I²C speeds so the host
 * cannot misconfigure. */
#define I2C_HW_FREQ_STD           100000u       /* Standard-mode    */
#define I2C_HW_FREQ_FAST          400000u       /* Fast-mode        */
#define I2C_HW_FREQ_FAST_PLUS    1000000u       /* Fast-mode Plus   */

/* How many bytes the XFER op can move in one transaction. Matches
 * the RPBP payload budget (4 kB) minus the command envelope. */
#define I2C_HW_MAX_BUFFER         2048u

/*
 * Bring up both I²C buses with their default pins and 100 kHz
 * clock. Called exactly once from main() after pin_broker_init().
 * Returns 0 on success, -1 if any pin conflict prevents bring-up.
 */
int i2c_hw_init_all(void);

/*
 * Bring up a single instance. `sda_pin`/`scl_pin` must support the
 * I²C alternate function for the chosen controller (SDA = I2C0 on
 * the even lane, SCL = I2C0 on the odd lane — see the RP2350
 * datasheet §4 "IO user bank" for the mapping).
 *
 * Enables internal pull-ups alongside the board's external 2.2 kΩ
 * resistors so a disconnected Qwiic cable still presents a defined
 * idle state.
 */
int i2c_hw_init(i2c_hw_instance_t instance,
                uint8_t sda_pin, uint8_t scl_pin,
                uint32_t baudrate);

/*
 * Change the bus clock at runtime. Must be one of the
 * I2C_HW_FREQ_* values; any other value returns -1.
 */
int i2c_hw_set_baudrate(i2c_hw_instance_t instance, uint32_t baudrate);

/*
 * Current baud rate in Hz, or 0 for an uninitialised instance.
 */
uint32_t i2c_hw_baudrate(i2c_hw_instance_t instance);

/*
 * Single transaction on the bus:
 *
 *   tx_len > 0 && rx_len == 0  → pure write (STOP at end unless
 *                                no_stop requested).
 *   tx_len == 0 && rx_len > 0  → pure read.
 *   tx_len > 0 && rx_len > 0   → write, repeated-START, read (the
 *                                canonical register-access pattern).
 *
 * Returns the total number of bytes actually moved (tx_written +
 * rx_read, both 0 on pure-read / pure-write branches respectively)
 * on success, or a negative RPBP status code on failure.
 *
 * Recognised errors:
 *   -RPBP_EINVAL   bad instance, address, or buffer size
 *   -RPBP_ENODEV   slave did not acknowledge its address
 *   -RPBP_EIO      bus-level error (arbitration loss, bit error)
 *   -RPBP_ETIMEDOUT  slave clock-stretched past `timeout_us`
 */
int i2c_hw_xfer(i2c_hw_instance_t instance,
                uint8_t addr,
                const uint8_t *tx, size_t tx_len,
                uint8_t *rx, size_t rx_len,
                bool no_stop,
                uint32_t timeout_us);

/*
 * Minimal-cost presence check. Issues a 1-byte read; ACK→device
 * present (returns 0), NACK→device absent (returns -RPBP_ENODEV),
 * timeout→-RPBP_ETIMEDOUT.
 */
int i2c_hw_probe(i2c_hw_instance_t instance, uint8_t addr, uint32_t timeout_us);

/* Diagnostics counters, cheap to read. */
struct i2c_hw_stats {
    uint32_t transfers_ok;
    uint32_t nacks;              /* -RPBP_ENODEV on address byte    */
    uint32_t timeouts;           /* clock-stretch past timeout      */
    uint32_t bus_errors;         /* arbitration / bit-level         */
    uint32_t bytes_written;
    uint32_t bytes_read;
};

const struct i2c_hw_stats *i2c_hw_stats(i2c_hw_instance_t instance);

/* How many instances the firmware manages. The board header decides
 * via RPBRIDGE_HAVE_I2C1: rev-B (RP2350B QFN-80) exposes both buses;
 * Pi Pico 2 (RP2350A QFN-60) exposes only bus 0. */
#include "pico.h"
#ifndef RPBRIDGE_HAVE_I2C1
#  define RPBRIDGE_HAVE_I2C1 1
#endif
#if RPBRIDGE_HAVE_I2C1
#  define I2C_HW_NUM_INSTANCES    2u
#else
#  define I2C_HW_NUM_INSTANCES    1u
#endif

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_I2C_HW_H */
