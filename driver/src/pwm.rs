// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-pwm — auxiliary child driver that registers a pwm_chip.
//
// Exposes the RP2350 hardware PWM slices as one combined pwm_chip.
// Channel numbering is 1:1 with the firmware-side slice enumeration
// published in the capability map.

use kernel::prelude::*;

#[allow(dead_code)]
pub(crate) struct PwmChild;

#[allow(dead_code)]
impl PwmChild {
    pub(crate) fn probe() -> Result<Self> {
        pr_info!("rpbridge-pwm: probe (stub)\n");
        Ok(PwmChild)
    }
}
