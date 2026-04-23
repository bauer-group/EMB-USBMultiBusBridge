// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-i2c — auxiliary child driver that registers an i2c_adapter.
//
// Each instance maps to one of the RPBridge's two hardware I²C
// controllers (bus index from crate::cap). Transfers become RPBP
// CMD_REQUEST{I2C_XFER} messages on the shared vendor bulk pipe; the
// dispatch layer fans responses back to the waiting adapter.
//
// Functionality bits to implement:
//   - I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL
//   - clock-stretching timeout pass-through to the firmware
//   - burst mode flag for long EEPROM reads (selects DMA path on the MCU)

use kernel::prelude::*;

#[allow(dead_code)]
pub(crate) struct I2cChild;

#[allow(dead_code)]
impl I2cChild {
    pub(crate) fn probe() -> Result<Self> {
        pr_info!("rpbridge-i2c: probe (stub)\n");
        Ok(I2cChild)
    }
}
