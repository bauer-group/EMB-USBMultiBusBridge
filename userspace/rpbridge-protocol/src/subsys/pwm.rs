// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// PWM subsystem (RPBP 0x04) — CONFIGURE / SET_DUTY / START / STOP /
// GET_STATE.

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
    pub const CONFIGURE: u8 = 0x00;
    pub const SET_DUTY: u8 = 0x01;
    pub const START: u8 = 0x02;
    pub const STOP: u8 = 0x03;
    pub const GET_STATE: u8 = 0x04;
}

/// CONFIGURE flag: invert the polarity (CMOS-active-low loads).
pub const FLAG_INVERT: u8 = 1 << 0;

/// Decoded `op::GET_STATE` reply.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct State {
    /// Frequency in Hz; 0 if the slice has not been configured.
    pub freq_hz: u32,
    /// Duty cycle as Q0.16; `duty_q16 / 65536` is the fraction.
    pub duty_q16: u16,
    /// Slice currently outputting on the wire.
    pub running: bool,
    /// Polarity inverted relative to the canonical "high = on".
    pub invert: bool,
    /// Firmware-internal slice index.
    pub slice: u8,
    /// Firmware-internal channel-within-slice index.
    pub channel: u8,
}

/* ───── 0x00 CONFIGURE ──────────────────────────────────────────── */

/// `[pin, flags, duty_q16:u16le, freq_hz:u32le]` = 8 bytes.
pub fn build_configure(
    dst: &mut [u8],
    pin: u8,
    flags: u8,
    duty_q16: u16,
    freq_hz: u32,
) -> Result<usize, Error> {
    need(dst, 8)?;
    dst[0] = pin;
    dst[1] = flags;
    wr_u16_le(&mut dst[2..4], duty_q16);
    wr_u32_le(&mut dst[4..8], freq_hz);
    Ok(8)
}

/* ───── 0x01 SET_DUTY ───────────────────────────────────────────── */

pub fn build_set_duty(dst: &mut [u8], pin: u8, duty_q16: u16) -> Result<usize, Error> {
    need(dst, 3)?;
    dst[0] = pin;
    wr_u16_le(&mut dst[1..3], duty_q16);
    Ok(3)
}

/* ───── 0x02 START / 0x03 STOP ──────────────────────────────────── */

pub fn build_start(dst: &mut [u8], pin: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = pin;
    Ok(1)
}

pub fn build_stop(dst: &mut [u8], pin: u8) -> Result<usize, Error> {
    build_start(dst, pin)
}

/* ───── 0x04 GET_STATE ──────────────────────────────────────────── */

pub fn build_get_state(dst: &mut [u8], pin: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = pin;
    Ok(1)
}

/// `[freq_hz:u32le, duty_q16:u16le, running:u8, invert:u8,
///   slice:u8, channel:u8]` = 10 bytes.
pub fn parse_get_state(result: &[u8]) -> Result<State, Error> {
    need_exact(result, 10)?;
    Ok(State {
        freq_hz: rd_u32_le(&result[0..4]),
        duty_q16: u16::from_le_bytes([result[4], result[5]]),
        running: result[6] & 1 != 0,
        invert: result[7] & 1 != 0,
        slice: result[8],
        channel: result[9],
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn configure_layout() {
        let mut buf = [0u8; 12];
        build_configure(&mut buf, 30, FLAG_INVERT, 0x4000, 1000).unwrap();
        assert_eq!(
            &buf[..8],
            &[
                30,
                FLAG_INVERT,
                0x00,
                0x40, // 0x4000 LE
                0xE8,
                0x03,
                0x00,
                0x00, // 1000 LE
            ]
        );
    }

    #[test]
    fn get_state_round_trip() {
        let raw = [
            0xE8, 0x03, 0x00, 0x00, // 1000 Hz
            0x00, 0x80, // 0x8000
            1, 0, // running, !invert
            5, 1, // slice 5, channel 1
        ];
        let s = parse_get_state(&raw).unwrap();
        assert_eq!(
            s,
            State {
                freq_hz: 1000,
                duty_q16: 0x8000,
                running: true,
                invert: false,
                slice: 5,
                channel: 1,
            }
        );
    }
}
