/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 BAUER GROUP
 *
 * adc — RP2350 SAR ADC wrapper.
 *
 * Channel map (rev-B, see docs/drafts/rpbridge-hardware-design.md §6):
 *   ch 0..3  = GP26..GP29  (user headers A0..A3)
 *   ch 4     = on-chip temperature sensor
 *
 * Reference voltage assumption: VREF = ADC_AVDD = 3.3 V (see hardware
 * design §2.1 for the ferrite-filtered analog rail). Convert 12-bit
 * codes to millivolts with:
 *       mV = raw_12bit * 3300 / 4095
 *
 * Streaming / IIO mode (continuous sampling on core 1) lands with M6
 * once the core-1 scheduler is up.
 */

#ifndef RPBRIDGE_ADC_H
#define RPBRIDGE_ADC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pull RPBRIDGE_ADC_* from the active board header. Defaulted here
 * so the ADC subsystem can be compiled standalone; the board header
 * always overrides. */
#include "pico.h"

#ifndef RPBRIDGE_ADC_EXT_CHANNEL_COUNT
#  define RPBRIDGE_ADC_EXT_CHANNEL_COUNT 4u
#endif

#define ADC_HW_VREF_MV        3300u
#define ADC_HW_USER_CHANNELS  RPBRIDGE_ADC_EXT_CHANNEL_COUNT
#define ADC_HW_TEMP_CHANNEL   RPBRIDGE_ADC_EXT_CHANNEL_COUNT  /* on-chip sensor sits one past the last external */

/* One-time module init — enables the ADC block, claims GP26..GP29
 * as ANALOG on the broker, and wires them as ADC inputs. Returns 0
 * on success, -RPBP_EBUSY on pin conflict. */
int adc_hw_init(void);

/*
 * Read one channel. `channel` in [0..3] routes to the corresponding
 * user header. Channel 4 reads the on-chip temperature sensor (use
 * adc_hw_read_temp_cdegC() for decoded °C).
 *
 * Returns the raw 12-bit sample [0..4095] on success, or a negative
 * RPBP status on error.
 */
int adc_hw_read_raw(uint8_t channel);

/*
 * Convenience: read a user channel and return millivolts. Returns a
 * negative RPBP status on error (channel out of range, etc.).
 */
int adc_hw_read_mv(uint8_t channel);

/* Decoded temperature in hundredths of °C. The on-chip sensor's
 * slope is ~1.721 mV/°C with V_TEMP @ 27 °C = 706 mV
 * (RP2350 datasheet §12.4.5). Returns a negative RPBP status on
 * error. */
int adc_hw_read_temp_cdegC(int16_t *out);

uint32_t adc_hw_vref_mv(void);

#ifdef __cplusplus
}
#endif
#endif /* RPBRIDGE_ADC_H */
