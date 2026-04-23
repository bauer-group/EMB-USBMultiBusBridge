// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-spi — auxiliary child driver that registers an spi_controller.
//
// One instance per hardware SPI engine (two on the RP2354B). Userspace
// access via the standard `/dev/spidevN.M` character device — no
// private IOCTL surface, no proprietary library.

use kernel::prelude::*;

#[allow(dead_code)]
pub(crate) struct SpiChild;

#[allow(dead_code)]
impl SpiChild {
    pub(crate) fn probe() -> Result<Self> {
        pr_info!("rpbridge-spi: probe (stub)\n");
        Ok(SpiChild)
    }
}
