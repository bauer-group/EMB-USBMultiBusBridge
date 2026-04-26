// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// DMX-512 subsystem (RPBP 0x08) — 513-slot universe transmitter.

#![allow(
    missing_docs,
    clippy::cast_possible_truncation,
    clippy::cast_lossless,
    clippy::missing_errors_doc,
    clippy::doc_markdown,
    clippy::unreadable_literal,
    clippy::needless_pass_by_value
)]

use super::{need, need_exact, wr_u16_le};
use crate::Error;

pub mod op {
    pub const GET_INFO: u8 = 0x00;
    pub const SET_SLOTS: u8 = 0x01;
    pub const START: u8 = 0x02;
    pub const STOP: u8 = 0x03;
    pub const SET_BREAK: u8 = 0x04;
}

/// DMX-512 universe size: start-code byte + 512 slot bytes = 513.
pub const UNIVERSE_LEN: usize = 513;

/// Decoded `op::GET_INFO` reply.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Info {
    pub count: u8,
    pub pin: u8,
    pub running: bool,
    pub slot_count: u16,
    pub break_us: u16,
    pub mab_us: u16,
}

/* ───── 0x00 GET_INFO ───────────────────────────────────────────── */

/// `[count, pin, running, reserved, slot_count:u16le, break_us:u16le,
///   mab_us:u16le]` = 10 bytes.
pub fn parse_info(result: &[u8]) -> Result<Info, Error> {
    need_exact(result, 10)?;
    Ok(Info {
        count: result[0],
        pin: result[1],
        running: result[2] & 1 != 0,
        slot_count: u16::from_le_bytes([result[4], result[5]]),
        break_us: u16::from_le_bytes([result[6], result[7]]),
        mab_us: u16::from_le_bytes([result[8], result[9]]),
    })
}

/* ───── 0x01 SET_SLOTS ──────────────────────────────────────────── */

/// `[idx, offset:u16le, bytes[…]]`. Offset 0 = the start-code byte
/// (DMX requires this to be 0x00 for standard 8-bit-data packets).
pub fn build_set_slots(dst: &mut [u8], idx: u8, offset: u16, bytes: &[u8]) -> Result<usize, Error> {
    if (offset as usize) + bytes.len() > UNIVERSE_LEN {
        return Err(Error::OutOfRange((offset as u32) + bytes.len() as u32));
    }
    let total = 3 + bytes.len();
    need(dst, total)?;
    dst[0] = idx;
    wr_u16_le(&mut dst[1..3], offset);
    if !bytes.is_empty() {
        dst[3..3 + bytes.len()].copy_from_slice(bytes);
    }
    Ok(total)
}

/* ───── 0x02 START / 0x03 STOP ──────────────────────────────────── */

pub fn build_start(dst: &mut [u8], idx: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = idx;
    Ok(1)
}

pub fn build_stop(dst: &mut [u8], idx: u8) -> Result<usize, Error> {
    build_start(dst, idx)
}

/* ───── 0x04 SET_BREAK ──────────────────────────────────────────── */

/// `[idx, break_us:u16le, mab_us:u16le]` = 5 bytes. DMX-512 spec
/// minima: break ≥ 88 µs, MAB ≥ 8 µs. We don't enforce them in the
/// builder (firmware re-validates), but the comments document it.
pub fn build_set_break(
    dst: &mut [u8],
    idx: u8,
    break_us: u16,
    mab_us: u16,
) -> Result<usize, Error> {
    need(dst, 5)?;
    dst[0] = idx;
    wr_u16_le(&mut dst[1..3], break_us);
    wr_u16_le(&mut dst[3..5], mab_us);
    Ok(5)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn set_slots_layout() {
        let mut dst = [0u8; 16];
        let bytes = [0xFF, 0x80, 0x40];
        let n = build_set_slots(&mut dst, 0, 1, &bytes).unwrap();
        assert_eq!(n, 6);
        assert_eq!(&dst[..6], &[0, 1, 0, 0xFF, 0x80, 0x40]);
    }

    #[test]
    fn set_slots_rejects_oversize() {
        let mut dst = [0u8; 600];
        let bytes = [0u8; 200];
        // offset 400 + 200 = 600 > UNIVERSE_LEN (513)
        assert!(matches!(
            build_set_slots(&mut dst, 0, 400, &bytes),
            Err(Error::OutOfRange(_))
        ));
    }

    #[test]
    fn break_round_trip() {
        let mut dst = [0u8; 8];
        let n = build_set_break(&mut dst, 0, 92, 12).unwrap();
        assert_eq!(n, 5);
        assert_eq!(&dst[..5], &[0, 92, 0, 12, 0]);
    }
}
