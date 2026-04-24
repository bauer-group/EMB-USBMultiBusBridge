// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-pwm — RPBP PWM subsystem client + auxiliary pwm_chip
// child. Wire format in docs/protocol/subsystems/pwm.md.

use kernel::prelude::*;

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::pwm as pw};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Pwm as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /// Build CONFIGURE — `[pin][flags][duty_q16 LE16][freq_hz LE32]`.
    pub fn build_configure(
        dst: &mut [u8], pin: u8, duty_q16: u16, freq_hz: u32, invert: bool,
    ) -> Result<usize, i32> {
        let n = start(dst, pw::CONFIGURE)?;
        if dst.len() < n + 8 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        dst[n + 1] = if invert { pw::FLAG_INVERT } else { 0 };
        rpbp::wr_le16(&mut dst[n + 2..], duty_q16);
        rpbp::wr_le32(&mut dst[n + 4..], freq_hz);
        Ok(n + 8)
    }

    pub fn build_set_duty(dst: &mut [u8], pin: u8, duty_q16: u16) -> Result<usize, i32> {
        let n = start(dst, pw::SET_DUTY)?;
        if dst.len() < n + 3 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        rpbp::wr_le16(&mut dst[n + 1..], duty_q16);
        Ok(n + 3)
    }

    pub fn build_start_stop(dst: &mut [u8], pin: u8, start_: bool) -> Result<usize, i32> {
        let op = if start_ { pw::START } else { pw::STOP };
        let n = start(dst, op)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        Ok(n + 1)
    }

    pub fn build_get_state(dst: &mut [u8], pin: u8) -> Result<usize, i32> {
        let n = start(dst, pw::GET_STATE)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        Ok(n + 1)
    }

    pub struct PwmState {
        pub freq_hz:  u32,
        pub duty_q16: u16,
        pub running:  bool,
        pub invert:   bool,
        pub slice:    u8,
        pub channel:  u8,
    }

    pub fn parse_state(resp: &[u8]) -> Result<PwmState, i32> {
        if resp.len() < 10 { return Err(crate::errno::EPROTO); }
        Ok(PwmState {
            freq_hz:  rpbp::rd_le32(&resp[0..4]),
            duty_q16: rpbp::rd_le16(&resp[4..6]),
            running:  resp[6] != 0,
            invert:   resp[7] != 0,
            slice:    resp[8],
            channel:  resp[9],
        })
    }
}

use kernel::sync::Arc;
use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;

#[allow(dead_code)]
pub(crate) struct PwmChild {
    _state: Arc<RpbridgeState>,
    _idx:   u16,
}

#[allow(dead_code)]
impl PwmChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        pr_info!("rpbridge-pwm.{}: probe (chip wiring pending)\n", bus.idx);
        Ok(PwmChild { _state: state, _idx: bus.idx })
    }
}
