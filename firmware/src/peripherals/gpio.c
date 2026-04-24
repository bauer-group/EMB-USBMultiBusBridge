/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * User-GPIO subsystem — see gpio.h for the public contract.
 */

#include "gpio.h"
#include "pin_broker.h"
#include "rpbp/rpbp.h"
#include "io_control/io_control.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/time.h"

/*
 * Host-exposed pin whitelist, fixed by the board layout (rev-B user
 * headers). Kept in ascending GP-number order so gpio_hw_list() can
 * memcpy directly.
 */
static const uint8_t g_user_pins[GPIO_HW_NUM_USER_PINS] = {
    30, 31, 37, 38, 39, 40, 41, 42, 43, 44, 45,
};

/* Bitmap of pins the GPIO-user subsystem currently owns + their
 * direction. Indexed by GPIO number (0..47). */
static uint64_t g_owned_mask;
static uint64_t g_output_mask;

/* ───── Helpers ──────────────────────────────────────────────────────── */

static bool owned(uint8_t pin)
{
    return pin < PIN_BROKER_NUM_PINS &&
           ((g_owned_mask >> pin) & 1u) != 0u;
}

static bool is_output(uint8_t pin)
{
    return pin < PIN_BROKER_NUM_PINS &&
           ((g_output_mask >> pin) & 1u) != 0u;
}

/* ───── Public API ───────────────────────────────────────────────────── */

void gpio_hw_init(void)
{
    g_owned_mask  = 0u;
    g_output_mask = 0u;
}

bool gpio_hw_is_user_pin(uint8_t pin)
{
    for (size_t i = 0; i < GPIO_HW_NUM_USER_PINS; ++i) {
        if (g_user_pins[i] == pin) return true;
    }
    return false;
}

size_t gpio_hw_list(uint8_t *dst, size_t cap)
{
    size_t n = (cap < GPIO_HW_NUM_USER_PINS) ? cap : GPIO_HW_NUM_USER_PINS;
    for (size_t i = 0; i < n; ++i) dst[i] = g_user_pins[i];
    return GPIO_HW_NUM_USER_PINS;
}

int gpio_hw_configure(uint8_t pin,
                      enum gpio_hw_dir  dir,
                      enum gpio_hw_pull pull)
{
    if (!gpio_hw_is_user_pin(pin)) return -RPBP_ENOENT;

    enum pin_function fn = (dir == GPIO_HW_DIR_OUTPUT)
                               ? PIN_FUNC_GPIO_OUT
                               : PIN_FUNC_GPIO_IN;
    int rc = pin_broker_claim(pin, PIN_OWNER_GPIO_USER, fn);
    if (rc < 0) return rc;

    gpio_init(pin);
    gpio_set_dir(pin, dir == GPIO_HW_DIR_OUTPUT);

    gpio_disable_pulls(pin);
    switch (pull) {
    case GPIO_HW_PULL_UP:   gpio_pull_up  (pin); break;
    case GPIO_HW_PULL_DOWN: gpio_pull_down(pin); break;
    case GPIO_HW_PULL_NONE: default: break;
    }

    g_owned_mask |= (uint64_t)1u << pin;
    if (dir == GPIO_HW_DIR_OUTPUT)
        g_output_mask |=  (uint64_t)1u << pin;
    else
        g_output_mask &= ~((uint64_t)1u << pin);

    return 0;
}

int gpio_hw_set(uint8_t pin, bool level)
{
    if (!owned(pin) || !is_output(pin)) return -RPBP_EPERM;
    gpio_put(pin, level);
    return 0;
}

int gpio_hw_get(uint8_t pin)
{
    if (!owned(pin)) return -RPBP_EPERM;
    return gpio_get(pin) ? 1 : 0;
}

int gpio_hw_set_mask(uint64_t mask, uint64_t values)
{
    /* Only act on bits that we own AND are configured as OUTPUT.
     * Everything else silently stays unchanged. */
    uint64_t writable = mask & g_owned_mask & g_output_mask;
    if (writable == 0u) return 0;

    uint64_t ones  = writable & values;
    uint64_t zeros = writable & ~values;
    if (ones)  gpio_set_mask64(ones);
    if (zeros) gpio_clr_mask64(zeros);
    return 0;
}

uint64_t gpio_hw_get_mask(uint64_t mask)
{
    uint64_t readable = mask & g_owned_mask;
    if (readable == 0u) return 0u;
    return gpio_get_all64() & readable;
}

/* ───── Edge-watch (IRQ-driven) ──────────────────────────────────────── */

/*
 * Shared GPIO IRQ callback. Runs in NVIC context — MUST stay short.
 * We timestamp, build an event struct, push onto the io_control GPIO
 * ring. The pump on the main loop drains the ring and emits RPBP
 * EVENT frames at normal priority.
 */
static void gpio_irq_trampoline(uint gpio, uint32_t events)
{
    struct io_gpio_event ev = {
        .timestamp_us = (uint32_t)time_us_64(),
        .pin          = (uint8_t)gpio,
        /* EDGE_RISE is bit 3, EDGE_FALL is bit 2; encode resulting
         * level so the host doesn't have to decode event flags. */
        .level        = (events & GPIO_IRQ_EDGE_RISE) ? 1u : 0u,
    };
    (void)ipc_queue_push(io_control_gpio_queue(), &ev);
}

static bool g_irq_cb_installed;

int gpio_hw_watch(uint8_t pin, uint8_t edges)
{
    if (!owned(pin))     return -RPBP_EPERM;

    /* Install the shared callback exactly once. Subsequent watches
     * just enable per-pin IRQs. */
    if (!g_irq_cb_installed) {
        gpio_set_irq_callback(gpio_irq_trampoline);
        /* Enable the GPIO bank IRQ at the NVIC level for this core. */
        irq_set_enabled(IO_IRQ_BANK0, true);
        g_irq_cb_installed = true;
    }

    /* Translate our wire-encoded edge bits to SDK event masks. */
    uint32_t event_mask = 0u;
    if (edges & GPIO_HW_WATCH_RISING)  event_mask |= GPIO_IRQ_EDGE_RISE;
    if (edges & GPIO_HW_WATCH_FALLING) event_mask |= GPIO_IRQ_EDGE_FALL;

    /* Disable first so we rewrite instead of OR-ing into stale state. */
    gpio_set_irq_enabled(pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    if (event_mask != 0u) {
        gpio_set_irq_enabled(pin, event_mask, true);
    }
    return 0;
}

int gpio_hw_unwatch(uint8_t pin)
{
    if (!owned(pin)) return -RPBP_EPERM;
    gpio_set_irq_enabled(pin,
                         GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
                         false);
    return 0;
}
