/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * gpio — user-exposed GPIO subsystem on the 2×10 user header.
 *
 * Pin-whitelist (rev-B, see docs/drafts/rpbridge-hardware-design.md §7.9):
 *   GP30, GP31, GP37, GP38, GP39, GP40      — GPIO + PWM capable
 *   GP41, GP42, GP43, GP44, GP45            — GPIO only
 *
 * Pins are NOT claimed on init — the subsystem only grabs them on
 * CONFIGURE so a user who never touches GPIO doesn't accidentally
 * block the PWM subsystem from taking e.g. GP30 for a slice output.
 * The `pin_broker` arbitrates between GPIO-user and PWM / any other
 * claimant if the host tries to bring a pin online from two
 * subsystems at once.
 */

#ifndef RPBRIDGE_GPIO_H
#define RPBRIDGE_GPIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "pico.h"   /* board header → RPBRIDGE_USER_GPIO_PIN_COUNT */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RPBRIDGE_USER_GPIO_PIN_COUNT
#  define RPBRIDGE_USER_GPIO_PIN_COUNT 11u
#endif

#define GPIO_HW_NUM_USER_PINS  RPBRIDGE_USER_GPIO_PIN_COUNT

/*
 * Direction / pull enums — wire-compatible with
 * rpbp.h `enum rpbp_gpio_dir` and `enum rpbp_gpio_pull` by integer
 * value.
 */
enum gpio_hw_dir {
    GPIO_HW_DIR_INPUT  = 0,
    GPIO_HW_DIR_OUTPUT = 1,
};

enum gpio_hw_pull {
    GPIO_HW_PULL_NONE = 0,
    GPIO_HW_PULL_UP   = 1,
    GPIO_HW_PULL_DOWN = 2,
};

/* One-time module init — resets the driver's internal state but does
 * not touch any GPIO. Safe to call before pin_broker_init() if the
 * caller prefers that order; the broker is touched lazily. */
void gpio_hw_init(void);

/* Returns true if `pin` is on the host-accessible whitelist. */
bool gpio_hw_is_user_pin(uint8_t pin);

/*
 * Copy the whitelist of user-accessible pins into `dst` in ascending
 * GP-number order. Returns the pin count (always
 * GPIO_HW_NUM_USER_PINS; `cap` just bounds the copy).
 */
size_t gpio_hw_list(uint8_t *dst, size_t cap);

/*
 * Configure `pin` as INPUT or OUTPUT with the requested pull.
 * Claims the pin on the broker with the matching function tag. Returns
 * 0 on success, -RPBP_ENOENT if not on the whitelist, -RPBP_EBUSY if
 * another subsystem holds the pin.
 */
int gpio_hw_configure(uint8_t pin,
                      enum gpio_hw_dir  dir,
                      enum gpio_hw_pull pull);

/*
 * Write `level` (0 or 1) on a pin previously configured as OUTPUT.
 * Returns 0 on success, -RPBP_EPERM if the pin is not output-claimed
 * by this subsystem.
 */
int gpio_hw_set(uint8_t pin, bool level);

/*
 * Read the current level of a user-GPIO pin (any direction).
 * Returns 0 / 1 on success, -RPBP_EPERM if the pin is not claimed
 * by this subsystem.
 */
int gpio_hw_get(uint8_t pin);

/*
 * Atomically drive every pin whose bit is set in `mask` to the
 * corresponding bit in `values`. Bits that don't correspond to
 * OUTPUT-claimed user pins are silently ignored (so a host can blast
 * a wide value without pre-filtering). The u64 width spans the full
 * RP2350B GP0..GP47 range. Returns 0 on success.
 */
int gpio_hw_set_mask(uint64_t mask, uint64_t values);

/*
 * Read the levels of every pin in `mask`. Unclaimed pins are reported
 * as 0 in the returned bitmap.
 */
uint64_t gpio_hw_get_mask(uint64_t mask);

/*
 * Edge-watch — subscribe the GPIO IRQ to edge events on `pin`. Events
 * land in the io_control GPIO ring and are emitted on RPBP channel 1
 * (events) by the main-loop pump.
 *
 * `edges` is a bitmask:
 *   bit 0 — rising
 *   bit 1 — falling
 * Passing 0 is equivalent to unwatch().
 *
 * Requires the pin to already be INPUT-claimed by the GPIO subsystem.
 * Returns 0 on success, -RPBP_EPERM when the pin is not ours.
 */
#define GPIO_HW_WATCH_RISING   (1u << 0)
#define GPIO_HW_WATCH_FALLING  (1u << 1)
#define GPIO_HW_WATCH_BOTH     (GPIO_HW_WATCH_RISING | GPIO_HW_WATCH_FALLING)

int gpio_hw_watch  (uint8_t pin, uint8_t edges);
int gpio_hw_unwatch(uint8_t pin);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_GPIO_H */
