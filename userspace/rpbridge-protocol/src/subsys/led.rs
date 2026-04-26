// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// LED-strip subsystem (RPBP 0x07) — addressable LED strips
// (WS2812 family).

#![allow(
    missing_docs,
    clippy::cast_possible_truncation,
    clippy::cast_lossless,
    clippy::missing_errors_doc,
    clippy::doc_markdown,
    clippy::unreadable_literal,
    clippy::needless_pass_by_value
)]

use super::{need, need_at_least, need_exact, wr_u16_le};
use crate::Error;

pub mod op {
    pub const GET_INFO: u8 = 0x00;
    pub const CONFIGURE: u8 = 0x01;
    pub const WRITE: u8 = 0x02;
    pub const CLEAR: u8 = 0x03;
}

/// Pixel format. Matches the firmware's `RPBP_LED_FMT_*` constants.
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Format {
    /// 24-bit GRB (most WS2812 variants).
    Grb = 0,
    /// 24-bit RGB.
    Rgb = 1,
    /// 32-bit GRBW (SK6812 RGBW).
    Grbw = 2,
}

impl Format {
    /// Bytes-per-pixel implied by the format.
    #[must_use]
    pub const fn bytes_per_pixel(self) -> usize {
        match self {
            Self::Grb | Self::Rgb => 3,
            Self::Grbw => 4,
        }
    }
}

impl TryFrom<u8> for Format {
    type Error = Error;
    fn try_from(v: u8) -> Result<Self, Self::Error> {
        Ok(match v {
            0 => Self::Grb,
            1 => Self::Rgb,
            2 => Self::Grbw,
            _ => return Err(Error::OutOfRange(v as u32)),
        })
    }
}

/// Decoded `op::GET_INFO` reply.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Info {
    pub count: u8,
    pub pin: u8,
    pub format: Format,
    pub pixel_count: u16,
    pub max_pixels: u16,
}

/* ───── 0x00 GET_INFO ───────────────────────────────────────────── */

/// `[count, pin, format, reserved, pixel_count:u16le, max_pixels:u16le]`
/// = 8 bytes.
pub fn parse_info(result: &[u8]) -> Result<Info, Error> {
    need_exact(result, 8)?;
    Ok(Info {
        count: result[0],
        pin: result[1],
        format: Format::try_from(result[2])?,
        pixel_count: u16::from_le_bytes([result[4], result[5]]),
        max_pixels: u16::from_le_bytes([result[6], result[7]]),
    })
}

/* ───── 0x01 CONFIGURE ──────────────────────────────────────────── */

/// `[idx, format, pixel_count:u16le]` = 4 bytes.
pub fn build_configure(
    dst: &mut [u8],
    idx: u8,
    format: Format,
    pixel_count: u16,
) -> Result<usize, Error> {
    need(dst, 4)?;
    dst[0] = idx;
    dst[1] = format as u8;
    wr_u16_le(&mut dst[2..4], pixel_count);
    Ok(4)
}

/* ───── 0x02 WRITE ──────────────────────────────────────────────── */

/// `[idx, offset:u16le, pixel_data[…]]`. Caller is responsible for
/// pre-formatting `pixel_data` per the strip's `Format` (GRB / RGB
/// / GRBW byte order). Maximum payload is bounded by the wire:
/// `MAX_PAYLOAD_LEN - CMD_REQUEST prelude (3) - WRITE prelude (3)`.
pub fn build_write(
    dst: &mut [u8],
    idx: u8,
    offset: u16,
    pixel_data: &[u8],
) -> Result<usize, Error> {
    let total = 3 + pixel_data.len();
    need(dst, total)?;
    dst[0] = idx;
    wr_u16_le(&mut dst[1..3], offset);
    if !pixel_data.is_empty() {
        dst[3..3 + pixel_data.len()].copy_from_slice(pixel_data);
    }
    Ok(total)
}

/* ───── 0x03 CLEAR ──────────────────────────────────────────────── */

pub fn build_clear(dst: &mut [u8], idx: u8) -> Result<usize, Error> {
    need(dst, 1)?;
    dst[0] = idx;
    Ok(1)
}

/// Convenience: receive raw bytes via `Identity`-style trait hooks.
/// Used for callers that want to inspect partial payloads without
/// the strict size check above.
pub fn peek_idx(result: &[u8]) -> Result<u8, Error> {
    need_at_least(result, 1)?;
    Ok(result[0])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn format_bpp() {
        assert_eq!(Format::Grb.bytes_per_pixel(), 3);
        assert_eq!(Format::Rgb.bytes_per_pixel(), 3);
        assert_eq!(Format::Grbw.bytes_per_pixel(), 4);
    }

    #[test]
    fn info_round_trip() {
        let raw = [1, 6, 1, 0, 0x1E, 0x00, 0xFF, 0x00];
        let info = parse_info(&raw).unwrap();
        assert_eq!(
            info,
            Info {
                count: 1,
                pin: 6,
                format: Format::Rgb,
                pixel_count: 30,
                max_pixels: 255,
            }
        );
    }

    #[test]
    fn write_layout() {
        let mut dst = [0u8; 16];
        let pixels = [0x80, 0x00, 0x00, 0x00, 0x80, 0x00];
        let n = build_write(&mut dst, 0, 0, &pixels).unwrap();
        assert_eq!(n, 9);
        assert_eq!(&dst[..3], &[0, 0, 0]);
        assert_eq!(&dst[3..9], &pixels);
    }
}
