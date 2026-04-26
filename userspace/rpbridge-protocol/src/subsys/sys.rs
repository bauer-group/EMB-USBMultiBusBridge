// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// SYS subsystem (RPBP 0x00) — control / identity / housekeeping.

#![allow(
    missing_docs,
    clippy::cast_possible_truncation,
    clippy::cast_lossless,
    clippy::missing_errors_doc,
    clippy::doc_markdown,
    clippy::unreadable_literal,
    clippy::needless_pass_by_value
)]

use super::{need_at_least, need_exact, rd_u32_le};
use crate::Error;

/// Opcode bytes mirrored from firmware/src/rpbp/rpbp.h.
pub mod op {
    pub const GET_CAPABILITIES: u8 = 0x00;
    pub const ECHO: u8 = 0x01;
    pub const REBOOT_BOOTSEL: u8 = 0x02;
    pub const UPTIME: u8 = 0x03;
    pub const GET_VBUS_MV: u8 = 0x04;
    pub const SET_LED: u8 = 0x05;
    pub const SELFTEST: u8 = 0x06;
    pub const GET_IDENTITY: u8 = 0x07;
    pub const RESET: u8 = 0x08;
}

/* ───── 0x03 UPTIME ────────────────────────────────────────────── */

/// Parse the result of `op::UPTIME` into milliseconds since boot.
pub fn parse_uptime_ms(result: &[u8]) -> Result<u64, Error> {
    need_exact(result, 8)?;
    Ok(u64::from_le_bytes([
        result[0], result[1], result[2], result[3], result[4], result[5], result[6], result[7],
    ]))
}

/* ───── 0x04 GET_VBUS_MV ───────────────────────────────────────── */

/// Parse the result of `op::GET_VBUS_MV` into millivolts.
pub fn parse_vbus_mv(result: &[u8]) -> Result<u32, Error> {
    need_exact(result, 4)?;
    Ok(rd_u32_le(result))
}

/* ───── 0x05 SET_LED ───────────────────────────────────────────── */

/// Builds the request body for `op::SET_LED`. Encoded as
/// `[r:u8, g:u8, b:u8, mode:u8]` where mode is firmware-private.
pub fn build_set_led(dst: &mut [u8], r: u8, g: u8, b: u8, mode: u8) -> Result<usize, Error> {
    super::need(dst, 4)?;
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    dst[3] = mode;
    Ok(4)
}

/* ───── 0x06 SELFTEST ──────────────────────────────────────────── */

/// Parse the result of `op::SELFTEST`. The firmware encodes the
/// pass/fail bitmask as a `u32` little-endian; bit N set = test N
/// failed. The caller maps bit positions back to test names.
pub fn parse_selftest_failures(result: &[u8]) -> Result<u32, Error> {
    need_exact(result, 4)?;
    Ok(rd_u32_le(result))
}

/* ───── 0x07 GET_IDENTITY ──────────────────────────────────────── */

/// Decoded device identity returned by `op::GET_IDENTITY`. Borrows
/// from the response buffer — caller must not retain past the
/// buffer's lifetime.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Identity<'a> {
    /// Firmware version triple (major, minor, patch).
    pub fw: [u8; 3],
    /// Hardware revision byte.
    pub board: u8,
    /// Serial number bytes (variable-length, up to 32 bytes).
    pub serial: &'a [u8],
}

/// Parse the result of `op::GET_IDENTITY`. Layout:
/// `[fw_major, fw_minor, fw_patch, board, serial_len:u8, serial[…]]`.
pub fn parse_identity(result: &[u8]) -> Result<Identity<'_>, Error> {
    need_at_least(result, 5)?;
    let serial_len = result[4] as usize;
    let want = 5 + serial_len;
    if result.len() < want {
        return Err(Error::BadResponseLen {
            expected: want,
            got: result.len(),
        });
    }
    Ok(Identity {
        fw: [result[0], result[1], result[2]],
        board: result[3],
        serial: &result[5..5 + serial_len],
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uptime_round_trip() {
        let buf = 0x_DEAD_BEEF_1234_5678u64.to_le_bytes();
        assert_eq!(parse_uptime_ms(&buf).unwrap(), 0x_DEAD_BEEF_1234_5678);
    }

    #[test]
    fn uptime_rejects_short() {
        let buf = [0u8; 4];
        assert!(matches!(
            parse_uptime_ms(&buf),
            Err(Error::BadResponseLen { .. })
        ));
    }

    #[test]
    fn set_led_layout() {
        let mut dst = [0u8; 8];
        let n = build_set_led(&mut dst, 0xAA, 0xBB, 0xCC, 0x01).unwrap();
        assert_eq!(n, 4);
        assert_eq!(&dst[..4], &[0xAA, 0xBB, 0xCC, 0x01]);
    }

    #[test]
    fn identity_with_serial() {
        let buf = [1, 2, 3, 4, 3, 0xDE, 0xAD, 0xBE];
        let id = parse_identity(&buf).unwrap();
        assert_eq!(id.fw, [1, 2, 3]);
        assert_eq!(id.board, 4);
        assert_eq!(id.serial, &[0xDE, 0xAD, 0xBE]);
    }

    #[test]
    fn identity_rejects_truncated_serial() {
        let buf = [1, 2, 3, 4, 5, 0xDE, 0xAD]; // claims 5 bytes, has 2
        assert!(matches!(
            parse_identity(&buf),
            Err(Error::BadResponseLen { .. })
        ));
    }
}
