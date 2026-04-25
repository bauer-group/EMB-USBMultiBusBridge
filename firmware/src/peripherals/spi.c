/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * RP2350 hardware_spi wrapper — see spi.h for the API contract.
 */

#include "spi.h"
#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include "hardware/spi.h"
#include "hardware/gpio.h"

#include <string.h>

/* Bus-line map per instance (rev-B pinout). */
struct spi_pins {
    uint8_t sck;
    uint8_t mosi;
    uint8_t miso;
};

static const struct spi_pins g_pins[SPI_HW_NUM_INSTANCES] = {
    { .sck  = RPBRIDGE_SPI0_SCK_PIN,
      .mosi = RPBRIDGE_SPI0_MOSI_PIN,
      .miso = RPBRIDGE_SPI0_MISO_PIN },   /* SPI0 — PMOD                    */
#if RPBRIDGE_HAVE_SPI1
    { .sck  = RPBRIDGE_SPI1_SCK_PIN,
      .mosi = RPBRIDGE_SPI1_MOSI_PIN,
      .miso = RPBRIDGE_SPI1_MISO_PIN },   /* SPI1 — Breakout (rev-B only)   */
#endif
};

struct spi_state {
    spi_inst_t *inst;
    uint32_t    freq_hz;
    uint8_t     mode_bits;
    bool        initialised;
};

static struct spi_state g_spi[SPI_HW_NUM_INSTANCES];

/*
 * Small LUT tracking which CS pins have been claimed so subsequent
 * XFERs don't re-hit the broker every call. Indexed by GPIO number
 * (0..47); 1 = we already hold it.
 */
static uint64_t g_cs_claimed_mask;

/* ───── Helpers ──────────────────────────────────────────────────────── */

static spi_inst_t *inst_for(spi_hw_instance_t i)
{
    return (i == 0u) ? spi0 : spi1;
}

static void apply_mode(spi_inst_t *inst, uint8_t bits)
{
    spi_cpol_t cpol = (bits & SPI_HW_MODE_CPOL) ? SPI_CPOL_1 : SPI_CPOL_0;
    spi_cpha_t cpha = (bits & SPI_HW_MODE_CPHA) ? SPI_CPHA_1 : SPI_CPHA_0;
    /* PL022 is MSB-only; LSB_FIRST is rejected by the caller. */
    spi_set_format(inst, 8, cpol, cpha, SPI_MSB_FIRST);
}

static bool cs_pin_claimed(uint8_t pin)
{
    return pin < 64u && ((g_cs_claimed_mask >> pin) & 1u) != 0u;
}

/* ───── Public API ───────────────────────────────────────────────────── */

int spi_hw_init_all(void)
{
    g_cs_claimed_mask = 0u;

    for (spi_hw_instance_t i = 0u; i < SPI_HW_NUM_INSTANCES; ++i) {
        struct spi_state *s = &g_spi[i];
        const struct spi_pins *p = &g_pins[i];

        if (pin_broker_claim(p->sck,  PIN_OWNER_SPI, PIN_FUNC_SPI_SCK)  < 0)
            return -RPBP_EBUSY;
        if (pin_broker_claim(p->mosi, PIN_OWNER_SPI, PIN_FUNC_SPI_MOSI) < 0)
            return -RPBP_EBUSY;
        if (pin_broker_claim(p->miso, PIN_OWNER_SPI, PIN_FUNC_SPI_MISO) < 0)
            return -RPBP_EBUSY;

        s->inst      = inst_for(i);
        s->freq_hz   = spi_init(s->inst, 1000000u);
        s->mode_bits = 0u;
        apply_mode(s->inst, s->mode_bits);

        gpio_set_function(p->sck,  GPIO_FUNC_SPI);
        gpio_set_function(p->mosi, GPIO_FUNC_SPI);
        gpio_set_function(p->miso, GPIO_FUNC_SPI);

        s->initialised = true;
    }
    return 0;
}

uint32_t spi_hw_set_freq(spi_hw_instance_t i, uint32_t freq_hz)
{
    if (i >= SPI_HW_NUM_INSTANCES)     return 0u;
    if (!g_spi[i].initialised)          return 0u;
    if (freq_hz == 0u)                  return 0u;

    /* hardware_spi clamps internally and returns the actual rate. */
    g_spi[i].freq_hz = spi_set_baudrate(g_spi[i].inst, freq_hz);
    return g_spi[i].freq_hz;
}

uint32_t spi_hw_freq(spi_hw_instance_t i)
{
    if (i >= SPI_HW_NUM_INSTANCES) return 0u;
    return g_spi[i].freq_hz;
}

int spi_hw_set_mode(spi_hw_instance_t i, uint8_t mode_bits)
{
    if (i >= SPI_HW_NUM_INSTANCES)      return -RPBP_EINVAL;
    if (!g_spi[i].initialised)           return -RPBP_EINVAL;
    if (mode_bits & SPI_HW_MODE_LSB_FIRST) return -RPBP_ENOTSUP;

    g_spi[i].mode_bits = mode_bits & 0x03u;
    apply_mode(g_spi[i].inst, g_spi[i].mode_bits);
    return 0;
}

uint8_t spi_hw_mode(spi_hw_instance_t i)
{
    if (i >= SPI_HW_NUM_INSTANCES) return 0u;
    return g_spi[i].mode_bits;
}

int spi_hw_cs_acquire(uint8_t cs_pin)
{
    if (cs_pin == 0xFFu)               return 0;
    if (cs_pin >= PIN_BROKER_NUM_PINS) return -RPBP_EINVAL;
    if (cs_pin_claimed(cs_pin))        return 0;

    int rc = pin_broker_claim(cs_pin, PIN_OWNER_SPI, PIN_FUNC_SPI_CS);
    if (rc < 0) return rc;

    gpio_init       (cs_pin);
    gpio_set_dir    (cs_pin, GPIO_OUT);
    gpio_put        (cs_pin, 1);          /* idle: deasserted */
    g_cs_claimed_mask |= (uint64_t)1u << cs_pin;
    return 0;
}

int spi_hw_cs_assert(uint8_t cs_pin)
{
    if (cs_pin == 0xFFu)         return 0;
    if (!cs_pin_claimed(cs_pin)) return -RPBP_EINVAL;
    gpio_put(cs_pin, 0);
    return 0;
}

int spi_hw_cs_release(uint8_t cs_pin)
{
    if (cs_pin == 0xFFu)         return 0;
    if (!cs_pin_claimed(cs_pin)) return -RPBP_EINVAL;
    gpio_put(cs_pin, 1);
    return 0;
}

int spi_hw_xfer(spi_hw_instance_t i,
                uint8_t cs_pin, bool hold_cs,
                const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (i >= SPI_HW_NUM_INSTANCES)   return -RPBP_EINVAL;
    if (!g_spi[i].initialised)       return -RPBP_EINVAL;
    if (len == 0u)                   return 0;
    if (len > (size_t)INT32_MAX)     return -RPBP_EMSGSIZE;

    if (cs_pin != 0xFFu) {
        int rc = spi_hw_cs_acquire(cs_pin);
        if (rc < 0) return rc;
        gpio_put(cs_pin, 0);
    }

    int n;
    if (tx != NULL && rx != NULL) {
        n = spi_write_read_blocking(g_spi[i].inst, tx, rx, len);
    } else if (tx != NULL) {
        n = spi_write_blocking(g_spi[i].inst, tx, len);
    } else if (rx != NULL) {
        n = spi_read_blocking(g_spi[i].inst, 0x00u, rx, len);
    } else {
        n = 0;
    }

    if (cs_pin != 0xFFu && !hold_cs) {
        gpio_put(cs_pin, 1);
    }

    if (n < 0)            return -RPBP_EIO;
    if ((size_t)n != len) return -RPBP_EIO;
    return n;
}
