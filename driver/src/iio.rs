// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-iio — RPBP ADC subsystem client + auxiliary iio_dev child.
// Wire format in docs/protocol/subsystems/adc.md.

use kernel::prelude::*;

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::adc as ad};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Adc as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 READ — [channel] → [raw LE16][mv LE16] */
    pub fn build_read(dst: &mut [u8], channel: u8) -> Result<usize, i32> {
        let n = start(dst, ad::READ)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = channel;
        Ok(n + 1)
    }

    /// (raw, millivolts) from a READ response.
    pub fn parse_read(resp: &[u8]) -> Result<(u16, u16), i32> {
        if resp.len() < 4 { return Err(crate::errno::EPROTO); }
        Ok((rpbp::rd_le16(&resp[0..2]), rpbp::rd_le16(&resp[2..4])))
    }

    /* 0x01 READ_MANY — [channel_mask LE16] */
    pub fn build_read_many(dst: &mut [u8], channel_mask: u16) -> Result<usize, i32> {
        let n = start(dst, ad::READ_MANY)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        rpbp::wr_le16(&mut dst[n..], channel_mask);
        Ok(n + 2)
    }

    pub struct ManySample { pub channel: u8, pub raw: u16, pub mv: u16 }

    pub fn parse_read_many(
        resp: &[u8], out: &mut Vec<ManySample>,
    ) -> Result<(), i32> {
        if resp.is_empty() { return Err(crate::errno::EPROTO); }
        let count = resp[0] as usize;
        let need = 1 + count * 5;
        if resp.len() < need { return Err(crate::errno::EPROTO); }
        out.clear();
        out.reserve(count, GFP_KERNEL)?;
        for i in 0..count {
            let o = 1 + i * 5;
            out.push(ManySample {
                channel: resp[o],
                raw:     rpbp::rd_le16(&resp[o + 1..o + 3]),
                mv:      rpbp::rd_le16(&resp[o + 3..o + 5]),
            }, GFP_KERNEL)?;
        }
        Ok(())
    }

    /* 0x02 GET_REF — → [vref_mv LE32] */
    pub fn build_get_ref(dst: &mut [u8]) -> Result<usize, i32> {
        start(dst, ad::GET_REF)
    }

    pub fn parse_vref_mv(resp: &[u8]) -> Result<u32, i32> {
        if resp.len() < 4 { return Err(crate::errno::EPROTO); }
        Ok(rpbp::rd_le32(&resp[..4]))
    }

    /* 0x03 TEMP_READ — → [cdegC LE16 i16] */
    pub fn build_temp_read(dst: &mut [u8]) -> Result<usize, i32> {
        start(dst, ad::TEMP_READ)
    }

    pub fn parse_temp_cdeg(resp: &[u8]) -> Result<i16, i32> {
        if resp.len() < 2 { return Err(crate::errno::EPROTO); }
        Ok(i16::from_le_bytes([resp[0], resp[1]]))
    }
}

use kernel::sync::Arc;
use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;

#[allow(dead_code)]
pub(crate) struct IioChild {
    _state: Arc<RpbridgeState>,
    _idx:   u16,
}

#[allow(dead_code)]
impl IioChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        pr_info!("rpbridge-iio.{}: probe (iio_dev wiring pending)\n", bus.idx);
        Ok(IioChild { _state: state, _idx: bus.idx })
    }
}
