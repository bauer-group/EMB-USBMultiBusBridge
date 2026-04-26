// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// I²S TX subsystem (RPBP 0x0A) — stereo PCM transmitter.

#![allow(
    missing_docs,
    clippy::cast_possible_truncation,
    clippy::cast_lossless,
    clippy::missing_errors_doc,
    clippy::doc_markdown,
    clippy::unreadable_literal,
    clippy::needless_pass_by_value
)]

use super::{need, need_exact, rd_u32_le, wr_u32_le};
use crate::Error;

pub mod op {
    pub const GET_INFO: u8 = 0x00;
    pub const CONFIGURE: u8 = 0x01;
    pub const START: u8 = 0x02;
    pub const STOP: u8 = 0x03;
    pub const WRITE: u8 = 0x04;
}

/// PCM-frame length: 16-bit stereo = 2 channels × 2 bytes = 4 B.
pub const FRAME_LEN: usize = 4;

/// Decoded `op::GET_INFO` reply.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Info {
    pub count: u8,
    pub bclk_pin: u8,
    pub lrclk_pin: u8,
    pub dout_pin: u8,
    pub sample_rate: u32,
    pub bit_depth: u8,
    pub running: bool,
}

/* ───── 0x00 GET_INFO ───────────────────────────────────────────── */

/// `[count, bclk, lrclk, dout, sample_rate:u32le, bit_depth, running,
///   reserved×2]` = 12 bytes.
pub fn parse_info(result: &[u8]) -> Result<Info, Error> {
    need_exact(result, 12)?;
    Ok(Info {
        count: result[0],
        bclk_pin: result[1],
        lrclk_pin: result[2],
        dout_pin: result[3],
        sample_rate: rd_u32_le(&result[4..8]),
        bit_depth: result[8],
        running: result[9] & 1 != 0,
    })
}

/* ───── 0x01 CONFIGURE ──────────────────────────────────────────── */

pub fn build_configure(dst: &mut [u8], idx: u8, sample_rate: u32) -> Result<usize, Error> {
    need(dst, 5)?;
    dst[0] = idx;
    wr_u32_le(&mut dst[1..5], sample_rate);
    Ok(5)
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

/* ───── 0x04 WRITE ──────────────────────────────────────────────── */

/// `[idx, pcm[…]]`. PCM length must be a multiple of [`FRAME_LEN`]
/// (4 B for 16-bit stereo); a half-frame ends ambiguously on the
/// firmware side and produces audible glitches, so we reject it
/// here pre-flight.
pub fn build_write(dst: &mut [u8], idx: u8, pcm: &[u8]) -> Result<usize, Error> {
    // is_multiple_of() is stable since 1.87 — workspace MSRV is 1.80,
    // so use the explicit modulo check that's available everywhere.
    if pcm.len() % FRAME_LEN != 0 {
        return Err(Error::OutOfRange(pcm.len() as u32));
    }
    let total = 1 + pcm.len();
    need(dst, total)?;
    dst[0] = idx;
    if !pcm.is_empty() {
        dst[1..=pcm.len()].copy_from_slice(pcm);
    }
    Ok(total)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn info_round_trip() {
        let raw = [
            1, 8, 9, 10, 0x44, 0xAC, 0x00, 0x00, // 44100 Hz LE
            16, 1, 0, 0,
        ];
        let info = parse_info(&raw).unwrap();
        assert_eq!(
            info,
            Info {
                count: 1,
                bclk_pin: 8,
                lrclk_pin: 9,
                dout_pin: 10,
                sample_rate: 44_100,
                bit_depth: 16,
                running: true,
            }
        );
    }

    #[test]
    fn write_rejects_unaligned() {
        let mut dst = [0u8; 16];
        let pcm = [0u8; 6]; // 1.5 frames
        assert!(matches!(
            build_write(&mut dst, 0, &pcm),
            Err(Error::OutOfRange(_))
        ));
    }

    #[test]
    fn write_layout() {
        let mut dst = [0u8; 16];
        let pcm = [0x00, 0x40, 0x00, 0x40]; // L=0x4000, R=0x4000
        let n = build_write(&mut dst, 0, &pcm).unwrap();
        assert_eq!(n, 5);
        assert_eq!(&dst[..5], &[0, 0x00, 0x40, 0x00, 0x40]);
    }
}
