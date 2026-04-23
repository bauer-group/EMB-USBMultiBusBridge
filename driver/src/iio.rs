// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-iio — auxiliary child driver that registers an iio_dev for
// the RP2350 ADC, exposing up to four external channels (GP26..GP29)
// plus the internal temperature sensor.
//
// Streaming mode is wired to a dedicated RPBP channel whose credits
// are managed by the core driver's flow-control logic.

use kernel::prelude::*;

#[allow(dead_code)]
pub(crate) struct IioChild;

#[allow(dead_code)]
impl IioChild {
    pub(crate) fn probe() -> Result<Self> {
        pr_info!("rpbridge-iio: probe (stub)\n");
        Ok(IioChild)
    }
}
