// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// GPIO subsystem (RPBP 0x03) — direction / pull / level / mask ops.

#![allow(
    missing_docs,
    clippy::cast_possible_truncation,
    clippy::cast_lossless,
    clippy::missing_errors_doc,
    clippy::doc_markdown,
    clippy::unreadable_literal,
    clippy::needless_pass_by_value
)]

use super::{need, need_at_least, need_exact, rd_u32_le, wr_u32_le};
use crate::Error;

pub mod op {
    pub const LIST: u8 = 0x00;
    pub const CONFIGURE: u8 = 0x01;
    pub const SET: u8 = 0x02;
    pub const GET: u8 = 0x03;
    pub const SET_MASK: u8 = 0x04;
    pub const GET_MASK: u8 = 0x05;
    pub const WATCH: u8 = 0x06;
    pub const UNWATCH: u8 = 0x07;
}

/// Pin direction.
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Dir {
    Input = 0,
    Output = 1,
}

/// Pin pull configuration.
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Pull {
    None = 0,
    Up = 1,
    Down = 2,
}

pub const WATCH_RISING: u8 = 1 << 0;
pub const WATCH_FALLING: u8 = 1 << 1;
pub const WATCH_BOTH: u8 = WATCH_RISING | WATCH_FALLING;

/* ───── 0x00 LIST ───────────────────────────────────────────────── */

/// Parse the LIST result: `[count:u8, pins[count]:u8]`.
pub fn parse_list(result: &[u8]) -> Result<&[u8], Error> {
    need_at_least(result, 1)?;
    let n = result[0] as usize;
    let want = 1 + n;
    if result.len() < want {
        return Err(Error::BadResponseLen {
            expected: want,
            got: result.len(),
        });
    }
    Ok(&result[1..want])
}

/* ───── 0x01 CONFIGURE ──────────────────────────────────────────── */

/// `[pin, dir, pull]` = 3 bytes.
pub fn build_configure(dst: &mut [u8], pin: u8, dir: Dir, pull: Pull) -> Result<usize, Error> {
    need(dst, 3)?;
    dst[0] = pin;
    dst[1] = dir as u8;
    dst[2] = pull as u8;
    Ok(3)
}

/* ───── 0x02 SET ────────────────────────────────────────────────── */

pub fn build_set(dst: &mut [u8], pin: u8, value: bool) -> Result<usize, Error> {
    need(dst, 2)?;
    dst[0] = pin;
    dst[1] = u8::from(value);
    Ok(2)
}

/* ───── 0x03 GET ────────────────────────────────────────────────── */

pub fn build_get(dst: &mut [u8], pin: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = pin;
    Ok(1)
}

pub fn parse_get(result: &[u8]) -> Result<bool, Error> {
    need_exact(result, 1)?;
    Ok(result[0] & 1 != 0)
}

/* ───── 0x04 SET_MASK ───────────────────────────────────────────── */

/// `[mask:u32le, values:u32le]` = 8 bytes.
pub fn build_set_mask(dst: &mut [u8], mask: u32, values: u32) -> Result<usize, Error> {
    need(dst, 8)?;
    wr_u32_le(&mut dst[0..4], mask);
    wr_u32_le(&mut dst[4..8], values);
    Ok(8)
}

/* ───── 0x05 GET_MASK ───────────────────────────────────────────── */

pub fn build_get_mask(dst: &mut [u8], mask: u32) -> Result<usize, Error> {
    need(dst, 4)?;
    wr_u32_le(&mut dst[0..4], mask);
    Ok(4)
}

pub fn parse_get_mask(result: &[u8]) -> Result<u32, Error> {
    need_exact(result, 4)?;
    Ok(rd_u32_le(result))
}

/* ───── 0x06 WATCH / 0x07 UNWATCH ───────────────────────────────── */

pub fn build_watch(dst: &mut [u8], pin: u8, edges: u8) -> Result<usize, Error> {
    if edges & !WATCH_BOTH != 0 {
        return Err(Error::OutOfRange(edges as u32));
    }
    need(dst, 2)?;
    dst[0] = pin;
    dst[1] = edges;
    Ok(2)
}

pub fn build_unwatch(dst: &mut [u8], pin: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = pin;
    Ok(1)
}

/// Decoded edge event from an EVENT-channel frame (channel 1).
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct EdgeEvent {
    pub pin: u8,
    pub level: u8,
    pub ts_us: u32,
}

/// Parse the body of an EVENT-channel frame whose first two bytes
/// match `[Subsys::Gpio, 0x00]` (the implicit "edge" event id).
pub fn parse_edge_event(payload: &[u8]) -> Result<EdgeEvent, Error> {
    need_at_least(payload, 8)?;
    if payload[0] != super::Subsys::Gpio as u8 || payload[1] != 0x00 {
        return Err(Error::OutOfRange(payload[0] as u32));
    }
    Ok(EdgeEvent {
        pin: payload[2],
        level: payload[3],
        ts_us: rd_u32_le(&payload[4..8]),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn configure_round_trip() {
        let mut buf = [0u8; 4];
        let n = build_configure(&mut buf, 7, Dir::Output, Pull::Up).unwrap();
        assert_eq!(n, 3);
        assert_eq!(&buf[..3], &[7, 1, 1]);
    }

    #[test]
    fn watch_rejects_bad_edges() {
        let mut buf = [0u8; 4];
        assert!(matches!(
            build_watch(&mut buf, 5, 0xFF),
            Err(Error::OutOfRange(_))
        ));
    }

    #[test]
    fn list_parser_walks_pin_array() {
        let buf = [3, 30, 31, 37];
        assert_eq!(parse_list(&buf).unwrap(), &[30, 31, 37]);
    }

    #[test]
    fn set_mask_is_le() {
        let mut buf = [0u8; 8];
        build_set_mask(&mut buf, 0x12345678, 0xAABBCCDD).unwrap();
        assert_eq!(buf, [0x78, 0x56, 0x34, 0x12, 0xDD, 0xCC, 0xBB, 0xAA]);
    }
}
