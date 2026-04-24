/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * pwm — RP2350 hardware PWM wrapper.
 *
 * The RP2350 has 12 PWM slices × 2 channels = 24 PWM outputs. Each
 * GPIO maps to a fixed slice/channel pair by the IO matrix (see the
 * RP2350 datasheet §5.11.4). This driver exposes:
 *
 *   - The four on-board status-LED channels (GP22/23/24 — PWM by the
 *     firmware, not by the user).
 *   - User-facing PWM channels on the 2×10 header: GP30, GP31, GP37,
 *     GP38, GP39, GP40 (6 user outputs).
 *
 * Duty is expressed as a Q0.16 fraction (`duty_q16` ∈ [0, 65535]).
 * 0 = 0 %, 65535 = 100 %, 32768 ≈ 50 %. Frequency is an integer Hz
 * value in [8, 125 000 000]. The requested rate is clamped; the
 * actual applied rate is readable via pwm_hw_state().
 *
 * Two pins that share a slice are NOT independent — the slice wrap
 * is global, so changing the frequency on one channel changes it on
 * its sibling. The driver rejects mutually-inconsistent configure
 * requests rather than silently co-aliasing.
 */

#ifndef RPBRIDGE_PWM_H
#define RPBRIDGE_PWM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWM_HW_NUM_USER_PINS  6u

enum pwm_hw_owner {
    PWM_HW_OWNER_FREE   = 0,
    PWM_HW_OWNER_LED    = 1,   /* on-board RGB status LED */
    PWM_HW_OWNER_USER   = 2,   /* RPBP PWM subsystem       */
};

struct pwm_hw_state {
    uint32_t freq_hz;         /* actual applied frequency          */
    uint16_t duty_q16;        /* actual applied duty (0..65535)    */
    uint8_t  running;         /* 0 = stopped, 1 = running          */
    uint8_t  invert;          /* 1 = channel polarity inverted     */
    uint8_t  slice;           /* PWM slice number                  */
    uint8_t  channel;         /* 0 (A) or 1 (B) inside the slice   */
};

/*
 * Initialise the driver. No pins are claimed here — slices come online
 * lazily on the first pwm_hw_configure()/pwm_hw_configure_led(). Safe
 * to call before pin_broker_init(). Idempotent.
 */
void pwm_hw_init(void);

/* Returns true if `pin` is on the host-accessible PWM whitelist. */
bool pwm_hw_is_user_pin(uint8_t pin);

size_t pwm_hw_user_list(uint8_t *dst, size_t cap);

/*
 * Configure a user PWM channel. Claims `pin` through the broker
 * (function = PIN_FUNC_PWM) and configures its slice to run at
 * `freq_hz` with `duty_q16` duty. `invert` flips the output polarity
 * at the GPIO output driver. Starts the slice.
 *
 * Returns 0 on success, -RPBP_ENOENT if `pin` is not on the PWM
 * whitelist, -RPBP_EBUSY on broker conflict, -RPBP_EINVAL on zero
 * frequency or a frequency that cannot be reached at all.
 */
int pwm_hw_configure(uint8_t  pin,
                     uint32_t freq_hz,
                     uint16_t duty_q16,
                     bool     invert);

/*
 * Claim a pin for the on-board LED driver at the same time as
 * configuring it. Identical semantics to pwm_hw_configure() but uses
 * PIN_OWNER_SYSTEM on the broker (pins GP22/23/24 are not on the
 * user whitelist, so the regular configure() rejects them).
 */
int pwm_hw_configure_led(uint8_t  pin,
                         uint32_t freq_hz,
                         uint16_t duty_q16);

/* Set a new duty on an already-configured pin. */
int pwm_hw_set_duty(uint8_t pin, uint16_t duty_q16);

/* Start / stop the slice that owns `pin`. stop() preserves the
 * configured parameters — a subsequent start() resumes with the same
 * freq and duty. */
int pwm_hw_start(uint8_t pin);
int pwm_hw_stop (uint8_t pin);

/* Query the current applied state of `pin`. Returns 0 on success,
 * -RPBP_ENOENT if the pin has not been configured. */
int pwm_hw_state(uint8_t pin, struct pwm_hw_state *out);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_PWM_H */
