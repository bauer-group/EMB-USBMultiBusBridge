// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// SPI subsystem (RPBP 0x02) — XFER / SET_MODE / freq / manual CS.

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
    pub const XFER: u8 = 0x00;
    pub const SET_MODE: u8 = 0x01;
    pub const SET_FREQ: u8 = 0x02;
    pub const GET_FREQ: u8 = 0x03;
    pub const CS_ASSERT: u8 = 0x04;
    pub const CS_RELEASE: u8 = 0x05;
}

/// XFER flag: keep CS asserted across this and the next XFER, so a
/// multi-transfer sequence stays atomic.
pub const FLAG_HOLD_CS: u8 = 1 << 0;

/// Mode bits for `SET_MODE`. `LSB_FIRST` is wire-level advertised
/// only — the firmware's PL022 controller rejects it (returns
/// `EINVAL` over the wire); kept here for parity with the spec.
pub const MODE_CPHA: u8 = 1 << 0;
pub const MODE_CPOL: u8 = 1 << 1;
pub const MODE_LSB_FIRST: u8 = 1 << 2;

/// Sentinel for "no manual CS" in the XFER body — userspace gpiolib
/// drives CS for the spidev path, so XFER ignores the cs_pin field
/// when this value is in slot 1.
pub const CS_PIN_NONE: u8 = 0xFF;

/* ───── 0x00 XFER ───────────────────────────────────────────────── */

/// `[instance, cs_pin, flags, reserved, tx_len:u16le, rx_len:u16le,
///   tx_data[tx_len]]` — 8-byte header + payload.
pub fn build_xfer(
    dst: &mut [u8],
    instance: u8,
    cs_pin: u8,
    flags: u8,
    tx: &[u8],
    rx_len: u16,
) -> Result<usize, Error> {
    if tx.len() > u16::MAX as usize {
        return Err(Error::OutOfRange(tx.len() as u32));
    }
    let tx_len = tx.len() as u16;
    let total = 8usize + tx.len();
    need(dst, total)?;
    dst[0] = instance;
    dst[1] = cs_pin;
    dst[2] = flags;
    dst[3] = 0; // reserved
    wr_u16_le(&mut dst[4..6], tx_len);
    wr_u16_le(&mut dst[6..8], rx_len);
    if !tx.is_empty() {
        dst[8..8 + tx.len()].copy_from_slice(tx);
    }
    Ok(total)
}

/* ───── 0x01 SET_MODE ───────────────────────────────────────────── */

pub fn build_set_mode(dst: &mut [u8], instance: u8, mode_bits: u8) -> Result<usize, Error> {
    need(dst, 2)?;
    dst[0] = instance;
    dst[1] = mode_bits;
    Ok(2)
}

/* ───── 0x02 SET_FREQ / 0x03 GET_FREQ ───────────────────────────── */

pub fn build_set_freq(dst: &mut [u8], instance: u8, freq_hz: u32) -> Result<usize, Error> {
    need(dst, 5)?;
    dst[0] = instance;
    wr_u32_le(&mut dst[1..5], freq_hz);
    Ok(5)
}

pub fn build_get_freq(dst: &mut [u8], instance: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = instance;
    Ok(1)
}

pub fn parse_get_freq(result: &[u8]) -> Result<u32, Error> {
    need_exact(result, 4)?;
    Ok(rd_u32_le(result))
}

/* ───── 0x04 CS_ASSERT / 0x05 CS_RELEASE ────────────────────────── */

pub fn build_cs(dst: &mut [u8], instance: u8, cs_pin: u8) -> Result<usize, Error> {
    need(dst, 2)?;
    dst[0] = instance;
    dst[1] = cs_pin;
    Ok(2)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn xfer_layout_full_duplex() {
        let mut dst = [0u8; 16];
        let tx = [0x12, 0x34];
        let n = build_xfer(&mut dst, 0, CS_PIN_NONE, 0, &tx, 2).unwrap();
        assert_eq!(n, 10);
        assert_eq!(
            &dst[..10],
            &[
                0,
                CS_PIN_NONE,
                0,
                0,
                2,
                0, // tx_len LE
                2,
                0, // rx_len LE
                0x12,
                0x34,
            ]
        );
    }

    #[test]
    fn set_mode_layout() {
        let mut dst = [0u8; 4];
        let n = build_set_mode(&mut dst, 1, MODE_CPHA | MODE_CPOL).unwrap();
        assert_eq!(n, 2);
        assert_eq!(&dst[..2], &[1, MODE_CPHA | MODE_CPOL]);
    }

    #[test]
    fn freq_round_trip() {
        let mut dst = [0u8; 8];
        build_set_freq(&mut dst, 0, 10_000_000).unwrap();
        assert_eq!(parse_get_freq(&dst[1..5]).unwrap(), 10_000_000);
    }
}
