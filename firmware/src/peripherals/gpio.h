/*
 * SPDX-License-Identifier: BSD-3-Clause
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

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_HW_NUM_USER_PINS  11u

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

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_GPIO_H */
