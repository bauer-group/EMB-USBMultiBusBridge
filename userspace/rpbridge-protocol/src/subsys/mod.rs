// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Per-subsystem wire builders and parsers for RPBP v1.
//
// Each submodule mirrors one firmware subsystem (mapped 1:1 to a
// `RPBP_SUBSYS_*` byte) and exposes:
//
//   - `pub const op::*`        — opcode bytes, name-for-name matched
//                                with `firmware/src/rpbp/rpbp.h`.
//   - `pub fn build_*(...)`    — encode a request payload (the bytes
//                                that follow the 3-byte CMD_REQUEST
//                                prelude `[subsys, opcode, status]`).
//   - `pub fn parse_*(...)`    — decode the result region of a
//                                CMD_RESPONSE payload (the bytes
//                                after the same 3-byte prelude).
//
// Errors surface as the crate-level `Error` enum from `lib.rs`
// (insufficient buffer, structural mismatch, etc.). All builders
// validate against the spec ceilings (`MAX_PAYLOAD_LEN`) and reject
// out-of-range values before composing the byte stream — no panics
// on hostile input.
//
// The submodules are deliberately independent: they share only the
// `op::` constant tables and a handful of helpers in this `mod.rs`.
// Each can be feature-gated separately later if `no_std` callers
// only want, say, the I²C path.

// The submodules' wire layers do byte-level encoding with intentional
// `as u32` truncations and `result[N]` indexing — both are
// clippy-pedantic offenders despite being correct here. The
// crate-level `#![warn(clippy::pedantic)]` would otherwise drown the
// log in mechanical lint warnings on review-clean code.
#![allow(
    clippy::module_name_repetitions,
    clippy::cast_possible_truncation,
    clippy::cast_lossless,
    clippy::missing_errors_doc,
    clippy::doc_markdown,
    clippy::unreadable_literal,
    missing_docs
)]

pub mod adc;
pub mod dmx;
pub mod gpio;
pub mod i2c;
pub mod i2s;
pub mod led;
pub mod onewire;
pub mod pwm;
pub mod spi;
pub mod sys;
pub mod uart;

/// Subsystem identifier byte that goes into the first slot of a
/// CMD_REQUEST or CMD_RESPONSE payload. Mirrors firmware
/// `enum rpbp_subsys`.
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub enum Subsys {
    /// System / control plane (capabilities, identity, reboot).
    Sys = 0x00,
    /// I²C master.
    I2c = 0x01,
    /// SPI controller.
    Spi = 0x02,
    /// GPIO bank.
    Gpio = 0x03,
    /// PWM generator.
    Pwm = 0x04,
    /// Analog-to-digital converter.
    Adc = 0x05,
    /// UART (PHY-mode only — byte stream goes via mainline cdc_acm).
    Uart = 0x06,
    /// Addressable LED strip (WS2812 family).
    Led = 0x07,
    /// DMX-512 transmitter.
    Dmx = 0x08,
    /// Maxim/Dallas 1-Wire master.
    Onewire = 0x09,
    /// I²S TX (raw PCM).
    I2s = 0x0A,
}

impl TryFrom<u8> for Subsys {
    type Error = crate::Error;

    fn try_from(v: u8) -> Result<Self, Self::Error> {
        Ok(match v {
            0x00 => Self::Sys,
            0x01 => Self::I2c,
            0x02 => Self::Spi,
            0x03 => Self::Gpio,
            0x04 => Self::Pwm,
            0x05 => Self::Adc,
            0x06 => Self::Uart,
            0x07 => Self::Led,
            0x08 => Self::Dmx,
            0x09 => Self::Onewire,
            0x0A => Self::I2s,
            _ => return Err(crate::Error::BadSubsys(v)),
        })
    }
}

/* ───── shared LE encode/decode helpers (private to subsys/) ─────── */

#[inline]
pub(crate) fn rd_u32_le(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

#[inline]
pub(crate) fn rd_i16_le(p: &[u8]) -> i16 {
    i16::from_le_bytes([p[0], p[1]])
}

#[inline]
pub(crate) fn wr_u16_le(p: &mut [u8], v: u16) {
    p[..2].copy_from_slice(&v.to_le_bytes());
}

#[inline]
pub(crate) fn wr_u32_le(p: &mut [u8], v: u32) {
    p[..4].copy_from_slice(&v.to_le_bytes());
}

/// Verify that `dst` is at least `n` bytes long; raises
/// `Error::BufferTooSmall` otherwise. Used at the top of every
/// builder so a single oversize-check pre-empts every per-byte
/// store.
#[inline]
pub(crate) fn need(dst: &[u8], n: usize) -> Result<(), crate::Error> {
    if dst.len() < n {
        Err(crate::Error::BufferTooSmall {
            needed: n,
            got: dst.len(),
        })
    } else {
        Ok(())
    }
}

/// Verify that `src` is exactly `n` bytes long; raises
/// `Error::BadResponseLen` otherwise. Used by parsers that expect
/// a fixed-size reply.
#[inline]
pub(crate) fn need_exact(src: &[u8], n: usize) -> Result<(), crate::Error> {
    if src.len() == n {
        Ok(())
    } else {
        Err(crate::Error::BadResponseLen {
            expected: n,
            got: src.len(),
        })
    }
}

/// Verify that `src` is at least `n` bytes long.
#[inline]
pub(crate) fn need_at_least(src: &[u8], n: usize) -> Result<(), crate::Error> {
    if src.len() < n {
        Err(crate::Error::BadResponseLen {
            expected: n,
            got: src.len(),
        })
    } else {
        Ok(())
    }
}
