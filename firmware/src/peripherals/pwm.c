/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * PWM — see pwm.h for the public contract.
 */

#include "pwm.h"
#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#include <string.h>

/* User-facing PWM pin whitelist — must match the board's 2×10 header
 * wiring (docs/drafts/rpbridge-hardware-design.md §7.9). */
static const uint8_t g_user_pins[PWM_HW_NUM_USER_PINS] = {
    30, 31, 37, 38, 39, 40,
};

/*
 * Per-pin state. Indexed by GPIO number (0..47).
 *
 * We track state per *pin* rather than per *slice* because two pins
 * sharing a slice may legitimately hold different duties (channel A vs
 * B) but the same frequency. Frequency changes therefore touch both
 * channels; duty changes only the one selected.
 */
struct pwm_pin_state {
    uint8_t  owner;     /* enum pwm_hw_owner              */
    uint8_t  slice;     /* slice 0..11                    */
    uint8_t  channel;   /* 0 = A, 1 = B                   */
    uint8_t  running;
    uint8_t  invert;
    uint8_t  reserved[3];
    uint32_t freq_hz;
    uint16_t wrap;
    uint16_t duty_q16;
};

static struct pwm_pin_state g_pins[PIN_BROKER_NUM_PINS];

/* ───── Clock / frequency math ───────────────────────────────────────── */

/*
 * Pick a (div_fp8, wrap) that yields an actual frequency closest to
 * `freq_hz`, given clk_sys. div_fp8 is the 12-bit fractional divider
 * packed as 16*int + frac (0x10 .. 0xFF0 inclusive → 1.0 .. 255.9375).
 *
 * Returns the actual rate achieved, or 0 if `freq_hz` is out of range.
 * Writes the chosen wrap to `*wrap_out` on success.
 */
static uint32_t solve_pwm(uint32_t freq_hz, uint16_t *wrap_out,
                          uint16_t *div_fp8_out)
{
    if (freq_hz == 0u) return 0u;

    uint64_t clk = clock_get_hz(clk_sys);
    /* ratio = clk / freq (with rounding). We want ratio == div * (wrap+1). */
    uint64_t ratio = (clk + (uint64_t)freq_hz / 2u) / (uint64_t)freq_hz;
    if (ratio < 1u) return 0u;

    /* Pick the smallest divider (best temporal resolution) such that
     * wrap fits in u16. div_fp8 is fixed-point 8.4: min 16 (= 1.0),
     * max 4080 (= 255.9375). wrap_reg = wrap - 1, we need wrap ≤ 0xFFFF. */
    uint32_t div_fp8 = 16u;
    uint32_t wrap    = (uint32_t)ratio - 1u;

    while (wrap > 0xFFFFu) {
        /* Double the divider until wrap fits. Could solve analytically
         * for perfect accuracy, but doubling keeps the quantisation
         * error below 50 % per step and the search ≤ 12 iterations. */
        div_fp8 *= 2u;
        if (div_fp8 > 4080u) {
            div_fp8 = 4080u;
            wrap    = (uint32_t)((clk * 16u) / ((uint64_t)div_fp8 * freq_hz));
            if (wrap == 0u || wrap > 0xFFFFu) return 0u;
            wrap -= 1u;
            break;
        }
        /* recompute wrap for the new divider. */
        wrap = (uint32_t)((clk * 16u) / ((uint64_t)div_fp8 * freq_hz));
        if (wrap == 0u) return 0u;
        wrap -= 1u;
    }

    *div_fp8_out = (uint16_t)div_fp8;
    *wrap_out    = (uint16_t)wrap;

    /* Actual rate = clk * 16 / (div_fp8 * (wrap+1)). */
    uint64_t actual = (clk * 16u) /
                      ((uint64_t)div_fp8 * ((uint64_t)wrap + 1u));
    return (uint32_t)actual;
}

static uint16_t duty_to_level(uint16_t duty_q16, uint16_t wrap)
{
    /* level = round(duty * (wrap+1) / 65536). */
    uint32_t level = ((uint32_t)duty_q16 * ((uint32_t)wrap + 1u) + 32768u)
                     >> 16;
    if (level > (uint32_t)wrap + 1u) level = (uint32_t)wrap + 1u;
    return (uint16_t)level;
}

/* ───── Low-level slice programming ──────────────────────────────────── */

static int do_configure(uint8_t pin, enum pwm_hw_owner owner,
                        uint32_t freq_hz, uint16_t duty_q16, bool invert)
{
    if (pin >= PIN_BROKER_NUM_PINS) return -RPBP_EINVAL;

    uint16_t wrap, div_fp8;
    uint32_t actual = solve_pwm(freq_hz, &wrap, &div_fp8);
    if (actual == 0u) return -RPBP_EINVAL;

    enum pin_owner broker_owner = (owner == PWM_HW_OWNER_LED)
                                      ? PIN_OWNER_SYSTEM
                                      : PIN_OWNER_PWM;
    int rc = pin_broker_claim(pin, broker_owner, PIN_FUNC_PWM);
    if (rc < 0) return rc;

    uint slice = pwm_gpio_to_slice_num(pin);
    uint chan  = pwm_gpio_to_channel  (pin);

    /* If the slice already hosts another pin with a different freq,
     * the request is physically unsatisfiable (slice wrap is shared).
     * Return EBUSY so the host knows to reconfigure both pins or pick
     * a different pair. */
    for (uint8_t p = 0u; p < PIN_BROKER_NUM_PINS; ++p) {
        if (p == pin) continue;
        if (g_pins[p].owner == PWM_HW_OWNER_FREE) continue;
        if (g_pins[p].slice != slice) continue;
        if (g_pins[p].freq_hz != actual) return -RPBP_EBUSY;
    }

    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac(&c, (uint8_t)(div_fp8 >> 4),
                                      (uint8_t)(div_fp8 & 0xFu));
    pwm_config_set_wrap(&c, wrap);
    pwm_init(slice, &c, false);   /* don't start yet */

    /* Level (duty). */
    uint16_t level = duty_to_level(duty_q16, wrap);
    pwm_set_chan_level(slice, chan, level);

    gpio_set_function(pin, GPIO_FUNC_PWM);
    gpio_set_outover(pin, invert ? GPIO_OVERRIDE_INVERT
                                 : GPIO_OVERRIDE_NORMAL);

    pwm_set_enabled(slice, true);

    g_pins[pin].owner    = (uint8_t)owner;
    g_pins[pin].slice    = (uint8_t)slice;
    g_pins[pin].channel  = (uint8_t)chan;
    g_pins[pin].running  = 1u;
    g_pins[pin].invert   = invert ? 1u : 0u;
    g_pins[pin].freq_hz  = actual;
    g_pins[pin].wrap     = wrap;
    g_pins[pin].duty_q16 = duty_q16;
    return 0;
}

/* ───── Public API ───────────────────────────────────────────────────── */

void pwm_hw_init(void)
{
    memset(g_pins, 0, sizeof(g_pins));
}

bool pwm_hw_is_user_pin(uint8_t pin)
{
    for (size_t i = 0; i < PWM_HW_NUM_USER_PINS; ++i) {
        if (g_user_pins[i] == pin) return true;
    }
    return false;
}

size_t pwm_hw_user_list(uint8_t *dst, size_t cap)
{
    size_t n = (cap < PWM_HW_NUM_USER_PINS) ? cap : PWM_HW_NUM_USER_PINS;
    for (size_t i = 0; i < n; ++i) dst[i] = g_user_pins[i];
    return PWM_HW_NUM_USER_PINS;
}

int pwm_hw_configure(uint8_t pin, uint32_t freq_hz,
                     uint16_t duty_q16, bool invert)
{
    if (!pwm_hw_is_user_pin(pin)) return -RPBP_ENOENT;
    return do_configure(pin, PWM_HW_OWNER_USER, freq_hz, duty_q16, invert);
}

int pwm_hw_configure_led(uint8_t pin, uint32_t freq_hz, uint16_t duty_q16)
{
    return do_configure(pin, PWM_HW_OWNER_LED, freq_hz, duty_q16, false);
}

int pwm_hw_set_duty(uint8_t pin, uint16_t duty_q16)
{
    if (pin >= PIN_BROKER_NUM_PINS)         return -RPBP_EINVAL;
    if (g_pins[pin].owner == PWM_HW_OWNER_FREE) return -RPBP_ENOENT;

    uint16_t level = duty_to_level(duty_q16, g_pins[pin].wrap);
    pwm_set_chan_level(g_pins[pin].slice, g_pins[pin].channel, level);
    g_pins[pin].duty_q16 = duty_q16;
    return 0;
}

int pwm_hw_start(uint8_t pin)
{
    if (pin >= PIN_BROKER_NUM_PINS)         return -RPBP_EINVAL;
    if (g_pins[pin].owner == PWM_HW_OWNER_FREE) return -RPBP_ENOENT;
    pwm_set_enabled(g_pins[pin].slice, true);
    g_pins[pin].running = 1u;
    return 0;
}

int pwm_hw_stop(uint8_t pin)
{
    if (pin >= PIN_BROKER_NUM_PINS)         return -RPBP_EINVAL;
    if (g_pins[pin].owner == PWM_HW_OWNER_FREE) return -RPBP_ENOENT;
    pwm_set_enabled(g_pins[pin].slice, false);
    g_pins[pin].running = 0u;
    return 0;
}

int pwm_hw_state(uint8_t pin, struct pwm_hw_state *out)
{
    if (out == NULL)                            return -RPBP_EINVAL;
    if (pin >= PIN_BROKER_NUM_PINS)             return -RPBP_EINVAL;
    if (g_pins[pin].owner == PWM_HW_OWNER_FREE) return -RPBP_ENOENT;

    out->freq_hz  = g_pins[pin].freq_hz;
    out->duty_q16 = g_pins[pin].duty_q16;
    out->running  = g_pins[pin].running;
    out->invert   = g_pins[pin].invert;
    out->slice    = g_pins[pin].slice;
    out->channel  = g_pins[pin].channel;
    return 0;
}
