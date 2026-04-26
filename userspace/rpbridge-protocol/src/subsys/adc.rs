// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// ADC subsystem (RPBP 0x05) — single-shot reads + on-die temperature.

#![allow(
    missing_docs,
    clippy::cast_possible_truncation,
    clippy::cast_lossless,
    clippy::missing_errors_doc,
    clippy::doc_markdown,
    clippy::unreadable_literal,
    clippy::needless_pass_by_value
)]

use super::{need, need_exact, rd_i16_le, rd_u32_le, wr_u16_le};
use crate::Error;

pub mod op {
    pub const READ: u8 = 0x00;
    pub const READ_MANY: u8 = 0x01;
    pub const GET_REF: u8 = 0x02;
    pub const TEMP_READ: u8 = 0x03;
    pub const STREAM_START: u8 = 0x04;
    pub const STREAM_STOP: u8 = 0x05;
}

/// Decoded `op::READ` reply: 12-bit raw + already-converted millivolts.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Sample {
    pub raw: u16,
    pub mv: u16,
}

/* ───── 0x00 READ ───────────────────────────────────────────────── */

pub fn build_read(dst: &mut [u8], channel: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = channel;
    Ok(1)
}

pub fn parse_read(result: &[u8]) -> Result<Sample, Error> {
    need_exact(result, 4)?;
    Ok(Sample {
        raw: u16::from_le_bytes([result[0], result[1]]),
        mv: u16::from_le_bytes([result[2], result[3]]),
    })
}

/* ───── 0x01 READ_MANY ──────────────────────────────────────────── */

/// Build a READ_MANY request: a 16-bit channel mask. Bit `n` set
/// requests channel `n`; the firmware replies in ascending channel
/// order with one (raw, mv) tuple per requested channel.
pub fn build_read_many(dst: &mut [u8], mask: u16) -> Result<usize, Error> {
    need(dst, 2)?;
    wr_u16_le(&mut dst[0..2], mask);
    Ok(2)
}

/// Parse READ_MANY reply: `[count:u8, (channel:u8 raw:u16le mv:u16le)*count]`.
/// Returns a borrowed slice over the per-channel tuples; the caller
/// walks them with `parse_read_many_entry`.
pub fn parse_read_many(result: &[u8]) -> Result<ReadManyIter<'_>, Error> {
    if result.is_empty() {
        return Err(Error::BadResponseLen {
            expected: 1,
            got: 0,
        });
    }
    let count = result[0] as usize;
    let want = 1 + count * 5;
    if result.len() < want {
        return Err(Error::BadResponseLen {
            expected: want,
            got: result.len(),
        });
    }
    Ok(ReadManyIter {
        rest: &result[1..=count * 5],
    })
}

/// Iterator over the per-channel records in a READ_MANY reply.
#[derive(Debug)]
pub struct ReadManyIter<'a> {
    rest: &'a [u8],
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ReadManyEntry {
    pub channel: u8,
    pub raw: u16,
    pub mv: u16,
}

impl Iterator for ReadManyIter<'_> {
    type Item = ReadManyEntry;
    fn next(&mut self) -> Option<Self::Item> {
        if self.rest.len() < 5 {
            return None;
        }
        let e = ReadManyEntry {
            channel: self.rest[0],
            raw: u16::from_le_bytes([self.rest[1], self.rest[2]]),
            mv: u16::from_le_bytes([self.rest[3], self.rest[4]]),
        };
        self.rest = &self.rest[5..];
        Some(e)
    }
}

/* ───── 0x02 GET_REF ────────────────────────────────────────────── */

pub fn parse_get_ref(result: &[u8]) -> Result<u32, Error> {
    need_exact(result, 4)?;
    Ok(rd_u32_le(result))
}

/* ───── 0x03 TEMP_READ ──────────────────────────────────────────── */

/// On-die temperature in centi-degrees Celsius (e.g. 2350 = 23.50 °C).
pub fn parse_temp_cdeg(result: &[u8]) -> Result<i16, Error> {
    need_exact(result, 2)?;
    Ok(rd_i16_le(result))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn read_round_trip() {
        let raw = [0x34, 0x0C, 0xCD, 0x0C]; // 0x0C34=3124 raw, 0x0CCD=3277 mv
        let s = parse_read(&raw).unwrap();
        assert_eq!(
            s,
            Sample {
                raw: 0x0C34,
                mv: 0x0CCD
            }
        );
    }

    #[test]
    fn temp_signed() {
        let raw = (-150i16).to_le_bytes(); // -1.5 °C
        assert_eq!(parse_temp_cdeg(&raw).unwrap(), -150);
    }

    #[test]
    fn read_many_walk() {
        let raw = [
            2, // count
            0, 0x10, 0x00, 0x20, 0x00, // ch0 raw=16 mv=32
            3, 0x00, 0x10, 0x00, 0x20, // ch3 raw=4096 mv=8192
        ];
        let mut it = parse_read_many(&raw).unwrap();
        assert_eq!(
            it.next().unwrap(),
            ReadManyEntry {
                channel: 0,
                raw: 16,
                mv: 32
            }
        );
        assert_eq!(
            it.next().unwrap(),
            ReadManyEntry {
                channel: 3,
                raw: 4096,
                mv: 8192
            }
        );
        assert!(it.next().is_none());
    }
}
