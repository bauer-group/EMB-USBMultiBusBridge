/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 BAUER GROUP
 *
 * ADC — see adc.h for the public contract.
 */

#include "adc.h"
#include "pin_broker.h"
#include "rpbp/rpbp.h"

#include "hardware/adc.h"

#include <stdint.h>

#define ADC_HW_FULL_SCALE     4095u    /* 12-bit SAR */

/* Per-channel pin map sourced from the active board header. RP2350A
 * and RP2350B both route the four ADC inputs to GP26..GP29, but the
 * indirection via macros keeps a future board with a different
 * ordering free to override. */
static const uint8_t k_adc_pins[ADC_HW_USER_CHANNELS] = {
    RPBRIDGE_ADC_CH0_PIN,
    RPBRIDGE_ADC_CH1_PIN,
    RPBRIDGE_ADC_CH2_PIN,
    RPBRIDGE_ADC_CH3_PIN,
};

static bool g_initialised;

int adc_hw_init(void)
{
    if (g_initialised) return 0;

    adc_init();
    for (uint8_t ch = 0; ch < ADC_HW_USER_CHANNELS; ++ch) {
        uint8_t pin = k_adc_pins[ch];
        int rc = pin_broker_claim(pin, PIN_OWNER_ADC, PIN_FUNC_GPIO_ANALOG);
        if (rc < 0) return rc;
        adc_gpio_init(pin);
    }
    adc_set_temp_sensor_enabled(true);

    g_initialised = true;
    return 0;
}

int adc_hw_read_raw(uint8_t channel)
{
    if (!g_initialised) return -RPBP_EINVAL;
    if (channel != ADC_HW_TEMP_CHANNEL && channel >= ADC_HW_USER_CHANNELS)
        return -RPBP_ENOENT;

    adc_select_input(channel);
    uint16_t raw = adc_read();
    return (int)raw;
}

int adc_hw_read_mv(uint8_t channel)
{
    int raw = adc_hw_read_raw(channel);
    if (raw < 0) return raw;
    return (raw * (int)ADC_HW_VREF_MV) / (int)ADC_HW_FULL_SCALE;
}

int adc_hw_read_temp_cdegC(int16_t *out)
{
    if (out == NULL) return -RPBP_EINVAL;

    int raw = adc_hw_read_raw(ADC_HW_TEMP_CHANNEL);
    if (raw < 0) return raw;

    /*
     * RP2350 datasheet §12.4.5:
     *   V_temp @ 27 °C  ≈ 0.706 V  (706 mV)
     *   sensitivity     ≈ 1.721 mV / °C (negative slope)
     *
     *   T[°C] = 27 - (V_mV - 706) / 1.721
     *
     * Computed in centi-degrees (0.01 °C) units with integer math:
     *   T_cdegC = 2700 - ((V_mV - 706) * 100000 / 1721)
     */
    int v_mv = (raw * (int)ADC_HW_VREF_MV) / (int)ADC_HW_FULL_SCALE;
    long t = 2700L - (((long)v_mv - 706L) * 100000L) / 1721L;

    if (t < INT16_MIN) t = INT16_MIN;
    if (t > INT16_MAX) t = INT16_MAX;

    *out = (int16_t)t;
    return 0;
}

uint32_t adc_hw_vref_mv(void)
{
    return ADC_HW_VREF_MV;
}
