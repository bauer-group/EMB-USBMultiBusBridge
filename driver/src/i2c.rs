// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-i2c — RPBP I²C subsystem client + auxiliary i2c_adapter
// child. Wire format in docs/protocol/subsystems/i2c.md.

use kernel::prelude::*;

/* ═════ Layer 1 — wire (kernel-binding agnostic) ══════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::i2c as ic};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::I2c as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 PROBE */
    pub fn build_probe(dst: &mut [u8], bus: u8, addr: u8) -> Result<usize, i32> {
        let n = start(dst, ic::PROBE)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        dst[n] = bus;
        dst[n + 1] = addr;
        Ok(n + 2)
    }

    /* 0x01 XFER */
    pub fn build_xfer(
        dst: &mut [u8], bus: u8, addr: u8, flags: u8,
        tx: &[u8], rx_len: u16,
    ) -> Result<usize, i32> {
        if tx.len() > u16::MAX as usize { return Err(crate::errno::EINVAL); }
        let n = start(dst, ic::XFER)?;
        let need = n + 7 + tx.len();
        if dst.len() < need { return Err(crate::errno::EINVAL); }
        dst[n]     = bus;
        dst[n + 1] = addr;
        dst[n + 2] = flags;
        rpbp::wr_le16(&mut dst[n + 3..], tx.len() as u16);
        rpbp::wr_le16(&mut dst[n + 5..], rx_len);
        dst[n + 7..n + 7 + tx.len()].copy_from_slice(tx);
        Ok(need)
    }

    /// Parse an XFER response. Returns the RX byte slice from the
    /// reply. Reply layout: `[rx_len LE u16][rx_data...]`.
    pub fn parse_xfer<'a>(resp: &'a [u8]) -> Result<&'a [u8], i32> {
        if resp.len() < 2 { return Err(crate::errno::EPROTO); }
        let rx_len = rpbp::rd_le16(&resp[..2]) as usize;
        if resp.len() < 2 + rx_len { return Err(crate::errno::EPROTO); }
        Ok(&resp[2..2 + rx_len])
    }

    /* 0x02 SCAN */
    pub fn build_scan(dst: &mut [u8], bus: u8) -> Result<usize, i32> {
        let n = start(dst, ic::SCAN)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = bus;
        Ok(n + 1)
    }

    /// Parse a SCAN response (16-byte bitmap; bit(addr) set on ACK).
    pub fn parse_scan(resp: &[u8], out: &mut [bool; 128]) -> Result<(), i32> {
        if resp.len() < 16 { return Err(crate::errno::EPROTO); }
        for addr in 0..128u8 {
            let byte = resp[(addr >> 3) as usize];
            out[addr as usize] = (byte >> (addr & 7)) & 1 != 0;
        }
        Ok(())
    }

    /* 0x03 SET_FREQ / 0x04 GET_FREQ */
    pub fn build_set_freq(dst: &mut [u8], bus: u8, freq_hz: u32) -> Result<usize, i32> {
        let n = start(dst, ic::SET_FREQ)?;
        if dst.len() < n + 5 { return Err(crate::errno::EINVAL); }
        dst[n] = bus;
        rpbp::wr_le32(&mut dst[n + 1..], freq_hz);
        Ok(n + 5)
    }

    pub fn build_get_freq(dst: &mut [u8], bus: u8) -> Result<usize, i32> {
        let n = start(dst, ic::GET_FREQ)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = bus;
        Ok(n + 1)
    }

    pub fn parse_freq_u32(resp: &[u8]) -> Result<u32, i32> {
        if resp.len() < 4 { return Err(crate::errno::EPROTO); }
        Ok(rpbp::rd_le32(&resp[..4]))
    }
}

/* ═════ Layer 2 — auxiliary_device adapter (signature only) ══════ */

use kernel::sync::Arc;
use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;

#[allow(dead_code)]
pub(crate) struct I2cChild {
    _state: Arc<RpbridgeState>,
    _idx:   u16,
}

#[allow(dead_code)]
impl I2cChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        pr_info!("rpbridge-i2c.{}: probe (adapter wiring pending)\n", bus.idx);
        Ok(I2cChild { _state: state, _idx: bus.idx })
    }
}
