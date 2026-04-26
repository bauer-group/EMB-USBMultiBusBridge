// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// 1-Wire (Maxim/Dallas) subsystem (RPBP 0x09) — single-master,
// single-bus.

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
    pub const RESET: u8 = 0x01;
    pub const WRITE: u8 = 0x02;
    pub const READ: u8 = 0x03;
    pub const TRIPLET: u8 = 0x04;
}

/// Decoded `op::GET_INFO` reply.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Info {
    pub count: u8,
    pub pin: u8,
    /// 0 = standard speed (current firmware), 1 = overdrive (planned).
    pub speed: u8,
}

/* ───── 0x00 GET_INFO ───────────────────────────────────────────── */

/// `[count, pin, speed, reserved]` = 4 bytes.
pub fn parse_info(result: &[u8]) -> Result<Info, Error> {
    need_exact(result, 4)?;
    Ok(Info {
        count: result[0],
        pin: result[1],
        speed: result[2],
    })
}

/* ───── 0x01 RESET ──────────────────────────────────────────────── */

pub fn build_reset(dst: &mut [u8], idx: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = idx;
    Ok(1)
}

/// RESET reply: 1 byte. `1` = device presence pulse detected,
/// `0` = no slave on the bus.
pub fn parse_reset(result: &[u8]) -> Result<bool, Error> {
    need_exact(result, 1)?;
    Ok(result[0] & 1 != 0)
}

/* ───── 0x02 WRITE ──────────────────────────────────────────────── */

pub fn build_write(dst: &mut [u8], idx: u8, bytes: &[u8]) -> Result<usize, Error> {
    let total = 1 + bytes.len();
    need(dst, total)?;
    dst[0] = idx;
    if !bytes.is_empty() {
        dst[1..=bytes.len()].copy_from_slice(bytes);
    }
    Ok(total)
}

/* ───── 0x03 READ ───────────────────────────────────────────────── */

/// `[idx, len:u16le]` = 3 bytes. Caller's reply buffer must hold
/// at least `len` bytes.
pub fn build_read(dst: &mut [u8], idx: u8, len: u16) -> Result<usize, Error> {
    need(dst, 3)?;
    dst[0] = idx;
    wr_u16_le(&mut dst[1..3], len);
    Ok(3)
}

/* ───── 0x04 TRIPLET ────────────────────────────────────────────── */

/// SEARCH ROM step result. See Maxim AN187 §4 for the full state
/// machine; this struct decodes the response byte's three packed
/// bits and is consumed by callers walking the device-discovery
/// tree.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Triplet {
    /// Bit value that the bus reports as the "real" id bit at this
    /// position (the AND of all attached slaves' ids).
    pub id_bit: bool,
    /// Bit value that the bus reports as the complement (the AND of
    /// all attached slaves' inverted ids). When `id_bit == cmp_bit`
    /// the bus is split — caller picked one branch via the `dir`
    /// argument to TRIPLET.
    pub cmp_bit: bool,
    /// Direction the firmware actually drove on the wire after
    /// observing id_bit / cmp_bit. May differ from the caller's
    /// requested direction when the bus was unanimous.
    pub dir: bool,
}

/// `[idx, dir]` = 2 bytes.
pub fn build_triplet(dst: &mut [u8], idx: u8, dir: bool) -> Result<usize, Error> {
    need(dst, 2)?;
    dst[0] = idx;
    dst[1] = u8::from(dir);
    Ok(2)
}

/// TRIPLET reply: one byte packed as `bit0 = id_bit`, `bit1 = cmp_bit`,
/// `bit2 = dir-actually-driven`.
pub fn parse_triplet(result: &[u8]) -> Result<Triplet, Error> {
    need_exact(result, 1)?;
    let b = result[0];
    Ok(Triplet {
        id_bit: b & 0b001 != 0,
        cmp_bit: b & 0b010 != 0,
        dir: b & 0b100 != 0,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn write_layout() {
        let mut dst = [0u8; 16];
        let bytes = [0xCC, 0x44]; // SKIP_ROM + CONVERT_T (DS18B20)
        let n = build_write(&mut dst, 0, &bytes).unwrap();
        assert_eq!(n, 3);
        assert_eq!(&dst[..3], &[0, 0xCC, 0x44]);
    }

    #[test]
    fn read_with_len() {
        let mut dst = [0u8; 4];
        let n = build_read(&mut dst, 0, 9).unwrap(); // DS18B20 scratchpad
        assert_eq!(n, 3);
        assert_eq!(&dst[..3], &[0, 9, 0]);
    }

    #[test]
    fn triplet_decoding() {
        // id=1, cmp=0, dir=1  → byte = 0b101 = 5
        let t = parse_triplet(&[0b101]).unwrap();
        assert_eq!(
            t,
            Triplet {
                id_bit: true,
                cmp_bit: false,
                dir: true
            }
        );
    }

    #[test]
    fn reset_presence() {
        assert!(parse_reset(&[1]).unwrap());
        assert!(!parse_reset(&[0]).unwrap());
    }
}
