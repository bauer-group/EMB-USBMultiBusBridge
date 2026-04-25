/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * RP2350 hardware-I²C wrapper. Two buses, blocking-with-timeout
 * transfers, transparent NACK → ENODEV and clock-stretch → ETIMEDOUT
 * mapping.
 *
 * DMA is deliberately NOT wired in here. A burst even at 1 Mbaud
 * takes ~10 µs / byte — below the RPBP round-trip budget for every
 * reasonable payload. The DMA upgrade lands with M6 once a real
 * producer-consumer streaming path exists (logic capture); before
 * then it would be premature optimisation.
 */

#include "i2c_hw.h"
#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/error.h"

#include <string.h>

/* ───── State ────────────────────────────────────────────────────────── */

struct i2c_hw_state {
    i2c_inst_t              *inst;
    uint8_t                  sda_pin;
    uint8_t                  scl_pin;
    uint32_t                 baudrate;
    bool                     initialised;
    struct i2c_hw_stats      stats;
};

static struct i2c_hw_state g_buses[I2C_HW_NUM_INSTANCES];

/* ───── Helpers ──────────────────────────────────────────────────────── */

static inline i2c_inst_t *inst_ptr(i2c_hw_instance_t i)
{
    return (i == 0u) ? i2c0 : i2c1;
}

static bool baud_is_canonical(uint32_t baud)
{
    return baud == I2C_HW_FREQ_STD
        || baud == I2C_HW_FREQ_FAST
        || baud == I2C_HW_FREQ_FAST_PLUS;
}

static int map_sdk_error(int sdk_rc)
{
    if (sdk_rc >= 0)                         return sdk_rc;
    if (sdk_rc == PICO_ERROR_GENERIC)        return -RPBP_ENODEV;     /* NACK */
    if (sdk_rc == PICO_ERROR_TIMEOUT)        return -RPBP_ETIMEDOUT;
    return -RPBP_EIO;
}

/* ───── Init ─────────────────────────────────────────────────────────── */

int i2c_hw_init(i2c_hw_instance_t instance,
                uint8_t sda_pin, uint8_t scl_pin,
                uint32_t baudrate)
{
    if (instance >= I2C_HW_NUM_INSTANCES) return -1;
    if (!baud_is_canonical(baudrate))     return -1;

    struct i2c_hw_state *s = &g_buses[instance];
    memset(s, 0, sizeof(*s));
    s->inst    = inst_ptr(instance);
    s->sda_pin = sda_pin;
    s->scl_pin = scl_pin;

    /* Reserve the pins first — conflicts are firmware bugs that
     * should fail the init call, not produce a silent double-drive. */
    if (pin_broker_claim(sda_pin, PIN_OWNER_I2C, PIN_FUNC_I2C_SDA) < 0) return -1;
    if (pin_broker_claim(scl_pin, PIN_OWNER_I2C, PIN_FUNC_I2C_SCL) < 0) return -1;

    i2c_init(s->inst, baudrate);

    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);

    /* External 2.2 kΩ pull-ups live on the board; the internal
     * pulls are kept on for the disconnected-Qwiic case so the bus
     * has a defined idle level. */
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);

    s->baudrate    = baudrate;
    s->initialised = true;
    return 0;
}

int i2c_hw_init_all(void)
{
    /* Pin map sourced from the board header. I²C #1 is conditional on
     * RPBRIDGE_HAVE_I2C1 — on RP2350A (Pi Pico 2) the pins for
     * bus 1 don't exist and we ship single-bus. */
    int rc0 = i2c_hw_init(0u,
                          RPBRIDGE_I2C0_SDA_PIN,
                          RPBRIDGE_I2C0_SCL_PIN,
                          I2C_HW_FREQ_STD);
#if RPBRIDGE_HAVE_I2C1
    int rc1 = i2c_hw_init(1u,
                          RPBRIDGE_I2C1_SDA_PIN,
                          RPBRIDGE_I2C1_SCL_PIN,
                          I2C_HW_FREQ_STD);
    return (rc0 == 0 && rc1 == 0) ? 0 : -1;
#else
    return rc0;
#endif
}

/* ───── Baud control ─────────────────────────────────────────────────── */

int i2c_hw_set_baudrate(i2c_hw_instance_t instance, uint32_t baudrate)
{
    if (instance >= I2C_HW_NUM_INSTANCES) return -1;
    struct i2c_hw_state *s = &g_buses[instance];
    if (!s->initialised)                  return -1;
    if (!baud_is_canonical(baudrate))     return -1;

    i2c_set_baudrate(s->inst, baudrate);
    s->baudrate = baudrate;
    return 0;
}

uint32_t i2c_hw_baudrate(i2c_hw_instance_t instance)
{
    if (instance >= I2C_HW_NUM_INSTANCES) return 0u;
    return g_buses[instance].baudrate;
}

/* ───── Transfer ─────────────────────────────────────────────────────── */

int i2c_hw_xfer(i2c_hw_instance_t instance,
                uint8_t addr,
                const uint8_t *tx, size_t tx_len,
                uint8_t *rx, size_t rx_len,
                bool no_stop,
                uint32_t timeout_us)
{
    if (instance >= I2C_HW_NUM_INSTANCES) return -RPBP_EINVAL;
    if (addr >= 0x80u)                    return -RPBP_EINVAL;   /* 10-bit addr via a later opcode */
    if (tx_len > I2C_HW_MAX_BUFFER)       return -RPBP_EMSGSIZE;
    if (rx_len > I2C_HW_MAX_BUFFER)       return -RPBP_EMSGSIZE;
    if (tx_len == 0 && rx_len == 0)       return -RPBP_EINVAL;

    struct i2c_hw_state *s = &g_buses[instance];
    if (!s->initialised)                  return -RPBP_EIO;

    int total = 0;

    if (tx_len > 0) {
        /* If a read follows we must NOT send STOP — a repeated
         * START ties the register-address write to the data read
         * without a bus free condition in between. */
        const bool write_nostop = (rx_len > 0) || no_stop;
        int rc = i2c_write_timeout_us(s->inst, addr,
                                      tx, (size_t)tx_len,
                                      write_nostop,
                                      timeout_us);
        if (rc < 0) {
            if (rc == PICO_ERROR_GENERIC)      s->stats.nacks++;
            else if (rc == PICO_ERROR_TIMEOUT) s->stats.timeouts++;
            else                               s->stats.bus_errors++;
            return map_sdk_error(rc);
        }
        s->stats.bytes_written += (uint32_t)rc;
        total += rc;
    }

    if (rx_len > 0) {
        int rc = i2c_read_timeout_us(s->inst, addr,
                                     rx, (size_t)rx_len,
                                     no_stop,
                                     timeout_us);
        if (rc < 0) {
            if (rc == PICO_ERROR_GENERIC)      s->stats.nacks++;
            else if (rc == PICO_ERROR_TIMEOUT) s->stats.timeouts++;
            else                               s->stats.bus_errors++;
            return map_sdk_error(rc);
        }
        s->stats.bytes_read += (uint32_t)rc;
        total += rc;
    }

    s->stats.transfers_ok++;
    return total;
}

/* ───── Probe ────────────────────────────────────────────────────────── */

int i2c_hw_probe(i2c_hw_instance_t instance, uint8_t addr, uint32_t timeout_us)
{
    if (instance >= I2C_HW_NUM_INSTANCES) return -RPBP_EINVAL;
    if (addr >= 0x80u)                    return -RPBP_EINVAL;

    struct i2c_hw_state *s = &g_buses[instance];
    if (!s->initialised)                  return -RPBP_EIO;

    /* A 1-byte read is the standard probe. ACK on address → present;
     * NACK → absent; timeout → stuck slave holding SDA low, useful
     * diagnostic on its own. The read byte itself is discarded. */
    uint8_t dummy = 0;
    int rc = i2c_read_timeout_us(s->inst, addr, &dummy, 1u, false, timeout_us);
    if (rc < 0) {
        if (rc == PICO_ERROR_GENERIC)      s->stats.nacks++;
        else if (rc == PICO_ERROR_TIMEOUT) s->stats.timeouts++;
        else                               s->stats.bus_errors++;
        return map_sdk_error(rc);
    }
    return 0;
}

/* ───── Stats ────────────────────────────────────────────────────────── */

const struct i2c_hw_stats *i2c_hw_stats(i2c_hw_instance_t instance)
{
    if (instance >= I2C_HW_NUM_INSTANCES) return NULL;
    return &g_buses[instance].stats;
}
