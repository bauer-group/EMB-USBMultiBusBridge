// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-spi — RPBP SPI subsystem client + auxiliary spi_controller
// child. Wire format in docs/protocol/subsystems/spi.md.

use kernel::prelude::*;

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::spi as sp};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Spi as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 XFER — args `[inst][cs_pin][flags][rsv][tx_len LE16][rx_len LE16][tx…]` */
    pub fn build_xfer(
        dst: &mut [u8], instance: u8, cs_pin: u8, flags: u8,
        tx: &[u8], rx_len: u16,
    ) -> Result<usize, i32> {
        let n = start(dst, sp::XFER)?;
        let need = n + 8 + tx.len();
        if dst.len() < need { return Err(crate::errno::EINVAL); }
        dst[n]     = instance;
        dst[n + 1] = cs_pin;
        dst[n + 2] = flags;
        dst[n + 3] = 0;
        rpbp::wr_le16(&mut dst[n + 4..], tx.len() as u16);
        rpbp::wr_le16(&mut dst[n + 6..], rx_len);
        dst[n + 8..n + 8 + tx.len()].copy_from_slice(tx);
        Ok(need)
    }

    pub fn parse_xfer<'a>(resp: &'a [u8]) -> Result<&'a [u8], i32> {
        if resp.len() < 2 { return Err(crate::errno::EPROTO); }
        let rx = rpbp::rd_le16(&resp[..2]) as usize;
        if resp.len() < 2 + rx { return Err(crate::errno::EPROTO); }
        Ok(&resp[2..2 + rx])
    }

    /* 0x01 SET_MODE — [inst][mode_bits] */
    pub fn build_set_mode(dst: &mut [u8], inst: u8, mode_bits: u8) -> Result<usize, i32> {
        let n = start(dst, sp::SET_MODE)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        dst[n] = inst;
        dst[n + 1] = mode_bits;
        Ok(n + 2)
    }

    /* 0x02 SET_FREQ / 0x03 GET_FREQ — [inst][freq LE32] / [inst] */
    pub fn build_set_freq(dst: &mut [u8], inst: u8, hz: u32) -> Result<usize, i32> {
        let n = start(dst, sp::SET_FREQ)?;
        if dst.len() < n + 5 { return Err(crate::errno::EINVAL); }
        dst[n] = inst;
        rpbp::wr_le32(&mut dst[n + 1..], hz);
        Ok(n + 5)
    }

    pub fn build_get_freq(dst: &mut [u8], inst: u8) -> Result<usize, i32> {
        let n = start(dst, sp::GET_FREQ)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = inst;
        Ok(n + 1)
    }

    pub fn parse_freq(resp: &[u8]) -> Result<u32, i32> {
        if resp.len() < 4 { return Err(crate::errno::EPROTO); }
        Ok(rpbp::rd_le32(&resp[..4]))
    }

    /* 0x04 CS_ASSERT / 0x05 CS_RELEASE — [cs_pin] */
    pub fn build_cs(dst: &mut [u8], cs_pin: u8, assert: bool) -> Result<usize, i32> {
        let op = if assert { sp::CS_ASSERT } else { sp::CS_RELEASE };
        let n = start(dst, op)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = cs_pin;
        Ok(n + 1)
    }
}

#[allow(dead_code)]
pub(crate) struct SpiChild;

#[allow(dead_code)]
impl SpiChild {
    pub(crate) fn probe() -> Result<Self> {
        pr_info!("rpbridge-spi: probe (wire layer ready; controller stub)\n");
        // TODO: register a kernel::spi::Controller with `transfer_one_message`.
        //   Iterate spi_message::xfers, call wire::build_xfer with HOLD_CS
        //   set on every xfer except the last (honour cs_change=0). Chip-
        //   selects drive gpio directly on the firmware via FLAG_HOLD_CS —
        //   the controller's native CS line handling is unused.
        Ok(SpiChild)
    }
}
