// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// I²C subsystem (RPBP 0x01) — XFER / SCAN / freq.

#![allow(
    missing_docs,
    clippy::cast_possible_truncation,
    clippy::cast_lossless,
    clippy::missing_errors_doc,
    clippy::doc_markdown,
    clippy::unreadable_literal,
    clippy::needless_pass_by_value
)]

use super::{need, need_exact, rd_u32_le, wr_u16_le, wr_u32_le};
use crate::Error;

pub mod op {
    pub const PROBE: u8 = 0x00;
    pub const XFER: u8 = 0x01;
    pub const SCAN: u8 = 0x02;
    pub const SET_FREQ: u8 = 0x03;
    pub const GET_FREQ: u8 = 0x04;
}

/// XFER flag: omit the trailing STOP so userspace can chain a
/// repeated-START sequence in subsequent calls.
pub const FLAG_NO_STOP: u8 = 1 << 0;

/* ───── 0x00 PROBE ──────────────────────────────────────────────── */

/// `[bus, addr]` = 2 bytes.
pub fn build_probe(dst: &mut [u8], bus: u8, addr: u8) -> Result<usize, Error> {
    need(dst, 2)?;
    dst[0] = bus;
    dst[1] = addr;
    Ok(2)
}

/// PROBE replies with one byte: 1 = ACK detected, 0 = NAK.
pub fn parse_probe(result: &[u8]) -> Result<bool, Error> {
    need_exact(result, 1)?;
    Ok(result[0] & 1 != 0)
}

/* ───── 0x01 XFER ───────────────────────────────────────────────── */

/// Build an XFER request body:
/// `[bus, addr, flags, tx_len:u16le, rx_len:u16le, tx_data[tx_len]]`.
pub fn build_xfer(
    dst: &mut [u8],
    bus: u8,
    addr: u8,
    flags: u8,
    tx: &[u8],
    rx_len: u16,
) -> Result<usize, Error> {
    if tx.len() > u16::MAX as usize {
        return Err(Error::OutOfRange(tx.len() as u32));
    }
    let tx_len = tx.len() as u16;
    let total = 7usize + tx.len();
    need(dst, total)?;
    dst[0] = bus;
    dst[1] = addr;
    dst[2] = flags;
    wr_u16_le(&mut dst[3..5], tx_len);
    wr_u16_le(&mut dst[5..7], rx_len);
    if !tx.is_empty() {
        dst[7..7 + tx.len()].copy_from_slice(tx);
    }
    Ok(total)
}

/* ───── 0x02 SCAN ───────────────────────────────────────────────── */

/// `[bus]` = 1 byte.
pub fn build_scan(dst: &mut [u8], bus: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = bus;
    Ok(1)
}

/// SCAN replies with a 16-byte bitmap of 7-bit addresses (128 bits).
/// Bit `addr` is set in `out[addr / 8]` if the device ACK'd.
pub fn parse_scan_bitmap(result: &[u8]) -> Result<&[u8; 16], Error> {
    // Length-check + array-conversion in one step. map_err collapses
    // the impossible-after-need_exact failure into the same variant
    // so we don't need a panicking unwrap.
    need_exact(result, 16)?;
    <&[u8; 16]>::try_from(result).map_err(|_| Error::BadResponseLen {
        expected: 16,
        got: result.len(),
    })
}

/* ───── 0x03 SET_FREQ / 0x04 GET_FREQ ───────────────────────────── */

pub fn build_set_freq(dst: &mut [u8], bus: u8, freq_hz: u32) -> Result<usize, Error> {
    need(dst, 5)?;
    dst[0] = bus;
    wr_u32_le(&mut dst[1..5], freq_hz);
    Ok(5)
}

pub fn build_get_freq(dst: &mut [u8], bus: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = bus;
    Ok(1)
}

pub fn parse_get_freq(result: &[u8]) -> Result<u32, Error> {
    need_exact(result, 4)?;
    Ok(rd_u32_le(result))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn xfer_rejects_oversize_tx() {
        let mut dst = [0u8; 16];
        let huge = vec![0u8; 70_000];
        assert!(matches!(
            build_xfer(&mut dst, 0, 0x42, 0, &huge, 0),
            Err(Error::OutOfRange(_))
        ));
    }

    #[test]
    fn xfer_layout_with_payload() {
        let mut dst = [0u8; 16];
        let tx = [0xAA, 0xBB, 0xCC];
        let n = build_xfer(&mut dst, 1, 0x42, FLAG_NO_STOP, &tx, 4).unwrap();
        assert_eq!(n, 10);
        assert_eq!(
            &dst[..10],
            &[
                1,
                0x42,
                FLAG_NO_STOP,
                3,
                0, // tx_len = 3 LE
                4,
                0, // rx_len = 4 LE
                0xAA,
                0xBB,
                0xCC,
            ]
        );
    }

    #[test]
    fn freq_round_trip() {
        let mut dst = [0u8; 8];
        build_set_freq(&mut dst, 0, 400_000).unwrap();
        let got = parse_get_freq(&dst[1..5]).unwrap();
        assert_eq!(got, 400_000);
    }
}
