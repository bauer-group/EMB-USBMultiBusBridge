// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// UART subsystem (RPBP 0x06) — PHY-mode control only.
//
// The byte stream itself goes through mainline cdc_acm (one CDC-ACM
// interface per UART). RPBP only carries the side-channel that
// switches the on-board level converter (TTL / RS-232 / RS-485).

#![allow(
    missing_docs,
    clippy::cast_possible_truncation,
    clippy::cast_lossless,
    clippy::missing_errors_doc,
    clippy::doc_markdown,
    clippy::unreadable_literal,
    clippy::needless_pass_by_value
)]

use super::{need, need_exact};
use crate::Error;

pub mod op {
    pub const SET_PHY: u8 = 0x00;
    pub const GET_PHY: u8 = 0x01;
}

/// Physical-layer mode for one UART instance. Mirrors the firmware's
/// `RPBP_UART_PHY_*` constants.
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Phy {
    /// Transceiver disabled, line floating. Default after probe.
    Off = 0,
    /// 3.3 V CMOS / TTL.
    Ttl = 1,
    /// RS-232 levels (driven by on-board MAX3232 on rev-B).
    Rs232 = 2,
    /// RS-485 half-duplex (driven by on-board SP3485 / MAX485).
    Rs485 = 3,
}

impl TryFrom<u8> for Phy {
    type Error = Error;

    fn try_from(v: u8) -> Result<Self, Self::Error> {
        Ok(match v {
            0 => Self::Off,
            1 => Self::Ttl,
            2 => Self::Rs232,
            3 => Self::Rs485,
            _ => return Err(Error::OutOfRange(v as u32)),
        })
    }
}

/* ───── 0x00 SET_PHY ────────────────────────────────────────────── */

pub fn build_set_phy(dst: &mut [u8], instance: u8, phy: Phy) -> Result<usize, Error> {
    need(dst, 2)?;
    dst[0] = instance;
    dst[1] = phy as u8;
    Ok(2)
}

/* ───── 0x01 GET_PHY ────────────────────────────────────────────── */

pub fn build_get_phy(dst: &mut [u8], instance: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = instance;
    Ok(1)
}

pub fn parse_get_phy(result: &[u8]) -> Result<Phy, Error> {
    need_exact(result, 1)?;
    Phy::try_from(result[0])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let mut dst = [0u8; 4];
        build_set_phy(&mut dst, 1, Phy::Rs485).unwrap();
        assert_eq!(&dst[..2], &[1, 3]);
        assert_eq!(parse_get_phy(&[3]).unwrap(), Phy::Rs485);
    }

    #[test]
    fn rejects_unknown_phy_byte() {
        assert!(matches!(Phy::try_from(99), Err(Error::OutOfRange(_))));
    }
}
