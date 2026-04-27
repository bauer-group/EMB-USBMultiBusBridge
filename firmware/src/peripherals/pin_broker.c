/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * pin_broker — allocation ledger for the RP2354B's 48 GPIOs.
 *
 * The broker is a passive data structure: callers that already
 * `gpio_init()` their pins through the pico-sdk add a matching
 * pin_broker_claim() so a second module attempting the same pin
 * gets EBUSY rather than silently fighting for the pad.
 *
 * Thread safety: the firmware serialises every claim/release
 * through the main loop and the control-channel dispatcher, which
 * all run on Core 0. The broker therefore does not take locks. If
 * that ever changes (Core 1 wants to claim runtime pins), swap the
 * simple writes for critical_section_enter/leave.
 */

#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include <string.h>

struct pin_slot {
    uint8_t owner;        /* enum pin_owner */
    uint8_t function;     /* enum pin_function */
};

static struct pin_slot g_pins[PIN_BROKER_NUM_PINS];

/* ───── Public API ───────────────────────────────────────────────────── */

void pin_broker_init(void)
{
    memset(g_pins, 0, sizeof(g_pins));
}

int pin_broker_claim(uint8_t pin, enum pin_owner owner, enum pin_function function)
{
    if (pin >= PIN_BROKER_NUM_PINS) return -RPBP_EINVAL;
    if (owner == PIN_OWNER_NONE)    return -RPBP_EINVAL;

    struct pin_slot *s = &g_pins[pin];

    if (s->owner == PIN_OWNER_NONE) {
        s->owner    = (uint8_t)owner;
        s->function = (uint8_t)function;
        return 0;
    }

    if (s->owner == (uint8_t)owner) {
        /* Idempotent re-claim by the same owner updates the
         * function tag. Useful when a module reconfigures a pin
         * mid-session (e.g. phy switch). */
        s->function = (uint8_t)function;
        return 0;
    }

    /* Different owner already holds it. */
    return -RPBP_EBUSY;
}

int pin_broker_release(uint8_t pin, enum pin_owner owner)
{
    if (pin >= PIN_BROKER_NUM_PINS) return -RPBP_EINVAL;

    struct pin_slot *s = &g_pins[pin];

    if (s->owner == PIN_OWNER_NONE) {
        return 0;                  /* idempotent free */
    }
    if (s->owner != (uint8_t)owner) {
        return -RPBP_EBUSY;        /* prevent cross-module release */
    }

    s->owner    = PIN_OWNER_NONE;
    s->function = PIN_FUNC_UNKNOWN;
    return 0;
}

bool pin_broker_is_free(uint8_t pin)
{
    if (pin >= PIN_BROKER_NUM_PINS) return false;
    return g_pins[pin].owner == PIN_OWNER_NONE;
}

enum pin_owner pin_broker_owner(uint8_t pin)
{
    if (pin >= PIN_BROKER_NUM_PINS) return PIN_OWNER_NONE;
    return (enum pin_owner)g_pins[pin].owner;
}

enum pin_function pin_broker_function(uint8_t pin)
{
    if (pin >= PIN_BROKER_NUM_PINS) return PIN_FUNC_UNKNOWN;
    return (enum pin_function)g_pins[pin].function;
}

size_t pin_broker_snapshot(struct pin_broker_entry *dst, size_t cap)
{
    size_t n = (cap < PIN_BROKER_NUM_PINS) ? cap : PIN_BROKER_NUM_PINS;
    for (size_t i = 0; i < n; ++i) {
        dst[i].pin      = (uint8_t)i;
        dst[i].owner    = g_pins[i].owner;
        dst[i].function = g_pins[i].function;
        dst[i].reserved = 0;
    }
    return PIN_BROKER_NUM_PINS;       /* actual count, not copied */
}

/* ───── Human-readable names ─────────────────────────────────────────── */

static const char *const k_owner_names[] = {
    [PIN_OWNER_NONE]       = "free",
    [PIN_OWNER_SYSTEM]     = "system",
    [PIN_OWNER_DEBUG_UART] = "debug-uart",
    [PIN_OWNER_UART_HW]    = "uart-hw",
    [PIN_OWNER_UART_PIO]   = "uart-pio",
    [PIN_OWNER_UART_PHY]   = "uart-phy",
    [PIN_OWNER_I2C]        = "i2c",
    [PIN_OWNER_SPI]        = "spi",
    [PIN_OWNER_PWM]        = "pwm",
    [PIN_OWNER_ADC]        = "adc",
    [PIN_OWNER_GPIO_USER]  = "gpio-user",
    [PIN_OWNER_CAN]        = "can",
    [PIN_OWNER_ONE_WIRE]   = "1-wire",
    [PIN_OWNER_RESERVED]   = "reserved",
};

static const char *const k_function_names[] = {
    [PIN_FUNC_UNKNOWN]     = "unknown",
    [PIN_FUNC_GPIO_IN]     = "gpio-in",
    [PIN_FUNC_GPIO_OUT]    = "gpio-out",
    [PIN_FUNC_GPIO_ANALOG] = "gpio-analog",
    [PIN_FUNC_UART_TX]     = "uart-tx",
    [PIN_FUNC_UART_RX]     = "uart-rx",
    [PIN_FUNC_I2C_SDA]     = "i2c-sda",
    [PIN_FUNC_I2C_SCL]     = "i2c-scl",
    [PIN_FUNC_SPI_SCK]     = "spi-sck",
    [PIN_FUNC_SPI_MOSI]    = "spi-mosi",
    [PIN_FUNC_SPI_MISO]    = "spi-miso",
    [PIN_FUNC_SPI_CS]      = "spi-cs",
    [PIN_FUNC_PWM]         = "pwm",
    [PIN_FUNC_PIO]         = "pio",
};

const char *pin_owner_name(enum pin_owner o)
{
    size_t n = sizeof(k_owner_names) / sizeof(k_owner_names[0]);
    if ((size_t)o >= n || k_owner_names[o] == NULL) return "unknown";
    return k_owner_names[o];
}

const char *pin_function_name(enum pin_function f)
{
    size_t n = sizeof(k_function_names) / sizeof(k_function_names[0]);
    if ((size_t)f >= n || k_function_names[f] == NULL) return "unknown";
    return k_function_names[f];
}
