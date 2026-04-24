/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * User-GPIO subsystem — see gpio.h for the public contract.
 */

#include "gpio.h"
#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include "hardware/gpio.h"

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
