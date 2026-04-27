/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * pin_broker — single source of truth for who owns which GPIO.
 *
 * Every module that takes a GPIO (a UART, an I²C, the status LED,
 * a user-exposed GPIO subsystem, …) calls pin_broker_claim() before
 * touching the pin. The broker refuses overlapping claims and
 * returns `-RPBP_EBUSY`, letting a misconfigured composite bring
 * itself down cleanly at init instead of silently co-driving the
 * same pad.
 *
 * The broker does NOT replace the pico-sdk's `gpio_init` +
 * `gpio_set_function` — it is pure bookkeeping. Modules still call
 * the SDK normally; the broker just tracks the allocation so
 * conflicts surface immediately and diagnostics
 * (`SYS SELFTEST`, capability map, `rpbridge-ctl pin list`) can
 * report who holds what.
 *
 * Pin-number range:
 *   The slot count is board-defined (RPBRIDGE_PIN_BROKER_SLOTS).
 *   RP2354B (QFN-80) exposes GP0..GP47 → 48 slots; RP2350A (QFN-60,
 *   used on Pi Pico 2) exposes GP0..GP29 → 30 slots. Pin numbers
 *   ≥ slot count are rejected with -RPBP_EINVAL.
 */

#ifndef RPBRIDGE_PIN_BROKER_H
#define RPBRIDGE_PIN_BROKER_H

#include "pico.h"   /* pulls in the active board header */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Defined by the active board header (firmware/boards/<board>.h).
 * Defaulted here so out-of-tree builds without our headers still
 * compile against the larger of the two supported packages. */
#ifndef RPBRIDGE_PIN_BROKER_SLOTS
#  define RPBRIDGE_PIN_BROKER_SLOTS 48u
#endif

#define PIN_BROKER_NUM_PINS    RPBRIDGE_PIN_BROKER_SLOTS

/*
 * Who is holding the pin. Identifying the owner lets the broker
 * accept idempotent re-claims (same owner claiming the same pin a
 * second time is a no-op) and lets diagnostics print something
 * meaningful.
 */
enum pin_owner {
    PIN_OWNER_NONE     = 0,   /* unclaimed */
    PIN_OWNER_SYSTEM,         /* LED, buttons, VBUS sense, reset */
    PIN_OWNER_DEBUG_UART,     /* stdio via UART1 on the debug header */
    PIN_OWNER_UART_HW,        /* UART0 HW TX/RX */
    PIN_OWNER_UART_PIO,       /* PIO-UART TX/RX */
    PIN_OWNER_UART_PHY,       /* RS485 EN, RS232 /SHDN, RS485 DE */
    PIN_OWNER_I2C,            /* SDA / SCL */
    PIN_OWNER_SPI,            /* SCK / MOSI / MISO / CS */
    PIN_OWNER_PWM,            /* PWM slice output */
    PIN_OWNER_ADC,            /* ADC channel input */
    PIN_OWNER_GPIO_USER,      /* generic GPIO exposed via the RPBP GPIO subsystem */
    PIN_OWNER_CAN,            /* PIO0 can2040 CAN-TX / CAN-RX */
    PIN_OWNER_ONE_WIRE,       /* 1-Wire data line */
    PIN_OWNER_RESERVED,       /* intentionally blocked */
};

/*
 * Broad function class the pin is used for. This is informative,
 * not authoritative — the RP2350 IO multiplexer still holds the
 * definitive alternate-function selection.
 */
enum pin_function {
    PIN_FUNC_UNKNOWN = 0,
    PIN_FUNC_GPIO_IN,
    PIN_FUNC_GPIO_OUT,
    PIN_FUNC_GPIO_ANALOG,
    PIN_FUNC_UART_TX,
    PIN_FUNC_UART_RX,
    PIN_FUNC_I2C_SDA,
    PIN_FUNC_I2C_SCL,
    PIN_FUNC_SPI_SCK,
    PIN_FUNC_SPI_MOSI,
    PIN_FUNC_SPI_MISO,
    PIN_FUNC_SPI_CS,
    PIN_FUNC_PWM,
    PIN_FUNC_PIO,             /* pin is under PIO control */
};

/*
 * Initialise the broker. Call exactly once at firmware start-up
 * before any module attempts to claim a pin. Resets the internal
 * ledger to all-unclaimed.
 */
void pin_broker_init(void);

/*
 * Claim `pin` for `owner` / `function`. Returns 0 on success,
 * -RPBP_EBUSY if a different owner already holds the pin, or
 * -RPBP_EINVAL if the pin number is out of range.
 *
 * Idempotent for the same `(pin, owner)` pair — re-claiming with
 * the same owner updates the `function` field and returns 0.
 */
int pin_broker_claim(uint8_t pin, enum pin_owner owner, enum pin_function function);

/*
 * Release `pin`. Returns 0 on success, -RPBP_EBUSY if `owner`
 * does not match the current holder (prevents one module from
 * clearing another's claim by mistake). Releasing an unclaimed
 * pin is a no-op and returns 0.
 */
int pin_broker_release(uint8_t pin, enum pin_owner owner);

/* Introspection helpers — never fail, 0 / UNKNOWN / NONE for an
 * out-of-range pin so callers can pass raw user input without
 * bounds-checking first. */
bool             pin_broker_is_free(uint8_t pin);
enum pin_owner   pin_broker_owner  (uint8_t pin);
enum pin_function pin_broker_function(uint8_t pin);

/*
 * Copy the current allocation table into `dst` (`cap` entries).
 * Returns the actual number of populated entries (always
 * PIN_BROKER_NUM_PINS; `cap` just bounds the copy). Intended for
 * the SYS SELFTEST payload and for `rpbridge-ctl pin list`.
 */
struct pin_broker_entry {
    uint8_t           pin;
    uint8_t           owner;      /* enum pin_owner */
    uint8_t           function;   /* enum pin_function */
    uint8_t           reserved;
};

size_t pin_broker_snapshot(struct pin_broker_entry *dst, size_t cap);

/* Human-readable name — NULL terminated, lives in flash. Unknown
 * ids return "unknown". */
const char *pin_owner_name   (enum pin_owner o);
const char *pin_function_name(enum pin_function f);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_PIN_BROKER_H */
