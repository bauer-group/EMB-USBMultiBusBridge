// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// rpbridge-protocol — RPBridge Protocol v1 (RPBP) framing library.
//
// This crate is the **single source of truth** for the wire format.
// Byte layout matches the firmware exactly (see
// firmware/src/rpbp/rpbp.h) and matches the kernel driver (see
// driver/src/rpbp.rs). All three implementations are exercised by the
// same golden-vector corpus under tests/golden/.

#![cfg_attr(not(feature = "std"), no_std)]
#![deny(missing_docs)]
#![warn(clippy::pedantic)]
#![allow(clippy::module_name_repetitions)]

//! # RPBP v1
//!
//! A frame on the wire consists of a fixed 16-byte header, an optional
//! payload of up to `MAX_PAYLOAD_LEN` bytes, and a trailing 4-byte
//! CRC-32C computed over `header || payload`. All multi-byte integer
//! fields are little-endian.

/// Capability-map decoder (CBOR over the SYS subsystem).
///
/// Mirrors `firmware/src/cap/capabilities.c` so the host can read
/// the device's advertised buses, features, and identity without
/// speaking CBOR directly. Available only with the `cbor` Cargo
/// feature.
#[cfg(feature = "cbor")]
pub mod cap;

mod crc32c;
pub use crc32c::{crc32c, crc32c_update, CRC32C_INIT};

// ─────────────────────────────────────────────────────────────────────
// Wire constants
// ─────────────────────────────────────────────────────────────────────

/// Start-of-frame marker byte (`'R'`).
pub const MAGIC: u8 = 0x52;

/// Major protocol version encoded in the header.
pub const VERSION: u8 = 0x01;

/// Size of the fixed RPBP header, in bytes.
pub const HEADER_LEN: usize = 16;

/// Size of the trailing CRC-32C, in bytes.
pub const CRC_LEN: usize = 4;

/// Largest payload length a v1 frame may carry. Senders MUST fragment
/// larger messages; receivers MUST reject frames that exceed this.
pub const MAX_PAYLOAD_LEN: usize = 4096;

/// Total maximum frame size on the wire (`HEADER_LEN + payload + CRC_LEN`).
pub const MAX_FRAME_LEN: usize = HEADER_LEN + MAX_PAYLOAD_LEN + CRC_LEN;

/// Canonical channel reserved for control-plane request/response.
pub const CHAN_CONTROL: u16 = 0;

/// Canonical channel reserved for unsolicited device events.
pub const CHAN_EVENTS: u16 = 1;

/// Broadcast channel — used sparingly for global device-side notices.
pub const CHAN_BROADCAST: u16 = 0xFFFF;

// ─────────────────────────────────────────────────────────────────────
// Message types
// ─────────────────────────────────────────────────────────────────────

/// Top-level RPBP message type. Integer values are **stable** across
/// minor versions of the protocol; new types are additive only.
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub enum MsgType {
    /// Session init, carries protocol version and identity.
    Hello = 0x00,
    /// CBOR capability map returned in response to a capability query.
    Capabilities = 0x01,
    /// Host → device synchronous command.
    CmdRequest = 0x02,
    /// Device → host response carrying the result of a `CmdRequest`.
    CmdResponse = 0x03,
    /// Payload frame on an open stream channel.
    StreamData = 0x04,
    /// Credit-grant for a stream channel (flow control).
    StreamCredit = 0x05,
    /// Unsolicited event (e.g. GPIO IRQ, bus fault).
    Event = 0x06,
    /// Round-trip measurement / keep-alive.
    Ping = 0x07,
    /// Response to `Ping`.
    Pong = 0x08,
    /// Error frame tied to a previous `seq` on the same `channel`.
    Error = 0x09,
    /// Flush/re-init the given channel.
    ResetChannel = 0x0A,
    /// Host ↔ device timestamp synchronisation (PTP-lite).
    TimeSync = 0x0B,
}

impl TryFrom<u8> for MsgType {
    type Error = Error;

    fn try_from(v: u8) -> Result<Self, Error> {
        use MsgType::{
            Capabilities, CmdRequest, CmdResponse, Error as E, Event, Hello, Ping, Pong,
            ResetChannel, StreamCredit, StreamData, TimeSync,
        };
        Ok(match v {
            0x00 => Hello,
            0x01 => Capabilities,
            0x02 => CmdRequest,
            0x03 => CmdResponse,
            0x04 => StreamData,
            0x05 => StreamCredit,
            0x06 => Event,
            0x07 => Ping,
            0x08 => Pong,
            0x09 => E,
            0x0A => ResetChannel,
            0x0B => TimeSync,
            _ => return Err(Error::BadMsgType(v)),
        })
    }
}

// ─────────────────────────────────────────────────────────────────────
// Flags
// ─────────────────────────────────────────────────────────────────────

/// Header flag bits. Bits 6 and 7 are reserved and MUST be zero in v1.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Default)]
pub struct Flags(u8);

impl Flags {
    /// Payload is CBOR-encoded.
    pub const CBOR: Flags = Flags(1 << 0);
    /// Payload is LZ4-block-compressed (only set when the capability
    /// map advertises `"compression": ["lz4"]`).
    pub const COMPRESSED: Flags = Flags(1 << 1);
    /// Urgent out-of-band marker (control channel only).
    pub const URGENT: Flags = Flags(1 << 2);
    /// Frame is a fragment and not the last one of its sequence.
    pub const FRAGMENT: Flags = Flags(1 << 3);
    /// Last fragment of a multi-frame message.
    pub const LAST: Flags = Flags(1 << 4);
    /// Continuation frame (middle of a multi-frame sequence).
    pub const CONTINUATION: Flags = Flags(1 << 5);

    /// Construct from raw bits, masking out reserved bits.
    #[must_use]
    pub const fn from_bits_truncate(bits: u8) -> Self {
        Self(bits & 0b0011_1111)
    }

    /// Raw header byte value.
    #[must_use]
    pub const fn bits(self) -> u8 {
        self.0
    }

    /// Returns `true` iff every bit in `other` is also set in `self`.
    #[must_use]
    pub const fn contains(self, other: Flags) -> bool {
        (self.0 & other.0) == other.0
    }
}

impl core::ops::BitOr for Flags {
    type Output = Flags;
    fn bitor(self, rhs: Flags) -> Flags {
        Flags(self.0 | rhs.0)
    }
}

impl core::ops::BitOrAssign for Flags {
    fn bitor_assign(&mut self, rhs: Flags) {
        self.0 |= rhs.0;
    }
}

// ─────────────────────────────────────────────────────────────────────
// Status codes
// ─────────────────────────────────────────────────────────────────────

/// POSIX-inspired status enum used inside `CmdResponse` and `Error`
/// payloads. The integer values are **stable** and MUST NOT be renumbered.
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
#[allow(missing_docs)]
pub enum Status {
    Ok = 0,
    EAgain = 1,
    EInval = 2,
    EPerm = 3,
    ENoEnt = 4,
    EIo = 5,
    ETimedOut = 6,
    EMsgSize = 7,
    ECrc = 8,
    EProto = 9,
    ENotSup = 10,
    EBusy = 11,
    ECanceled = 12,
    ENoSpc = 13,
    EOverflow = 14,
}

// ─────────────────────────────────────────────────────────────────────
// Errors
// ─────────────────────────────────────────────────────────────────────

/// Errors raised by parsing / validation. Variants mirror the most
/// common failure modes in `crate::Status` and include the offending
/// value so upstream error reporting can be precise.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Error {
    /// Buffer is shorter than `HEADER_LEN + CRC_LEN`.
    FrameTooShort,
    /// First byte is not `MAGIC` or version byte is not `VERSION`.
    BadMagicOrVersion,
    /// `payload_len` field claims more bytes than `MAX_PAYLOAD_LEN`.
    PayloadTooLarge,
    /// Buffer length disagrees with the header's `payload_len`.
    FrameLengthMismatch,
    /// CRC-32C on the wire does not match the computed value.
    BadCrc,
    /// `msg_type` field is not a known v1 message type.
    BadMsgType(u8),
    /// Reserved flag bits (6, 7) are set — reject strictly in v1.
    ReservedFlagBits(u8),
}

#[cfg(feature = "std")]
impl std::error::Error for Error {}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Error::FrameTooShort => write!(f, "frame shorter than header+CRC"),
            Error::BadMagicOrVersion => write!(f, "wrong magic or version byte"),
            Error::PayloadTooLarge => write!(f, "payload_len exceeds MAX_PAYLOAD_LEN"),
            Error::FrameLengthMismatch => write!(f, "frame length != header+payload_len+CRC"),
            Error::BadCrc => write!(f, "CRC-32C check failed"),
            Error::BadMsgType(v) => write!(f, "unknown msg_type 0x{v:02X}"),
            Error::ReservedFlagBits(b) => write!(f, "reserved flag bits set: 0x{b:02X}"),
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// Header
// ─────────────────────────────────────────────────────────────────────

/// Parsed header — the in-memory view of the 16 wire bytes.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Header {
    /// Start-of-frame marker. Must equal `MAGIC` on the wire.
    pub magic: u8,
    /// Protocol version byte. Must equal `VERSION` on the wire.
    pub version: u8,
    /// Message-type byte; decode with [`MsgType::try_from`].
    pub msg_type: u8,
    /// Flags byte. Reserved bits must be zero.
    pub flags: Flags,
    /// Logical channel (`CHAN_CONTROL`, `CHAN_EVENTS`, or dynamic).
    pub channel: u16,
    /// Per-channel monotonic sequence number.
    pub seq: u16,
    /// Payload length in bytes (excludes header + CRC).
    pub payload_len: u32,
    /// Device-boot microseconds when the frame was formed.
    pub timestamp_us: u32,
}

impl Header {
    /// Parse a 16-byte header from a byte slice. The slice may be
    /// longer than `HEADER_LEN`; only the first 16 bytes are consumed.
    ///
    /// # Errors
    ///
    /// Returns `Error::FrameTooShort` if the slice is shorter than 16
    /// bytes, `Error::BadMagicOrVersion` if the first two bytes are
    /// wrong, and `Error::PayloadTooLarge` if the declared payload
    /// exceeds `MAX_PAYLOAD_LEN`.
    pub fn parse(src: &[u8]) -> Result<Self, Error> {
        if src.len() < HEADER_LEN {
            return Err(Error::FrameTooShort);
        }
        if src[0] != MAGIC || src[1] != VERSION {
            return Err(Error::BadMagicOrVersion);
        }
        let raw_flags = src[3];
        if (raw_flags & 0b1100_0000) != 0 {
            return Err(Error::ReservedFlagBits(raw_flags));
        }
        let header = Header {
            magic: src[0],
            version: src[1],
            msg_type: src[2],
            flags: Flags::from_bits_truncate(raw_flags),
            channel: u16::from_le_bytes([src[4], src[5]]),
            seq: u16::from_le_bytes([src[6], src[7]]),
            payload_len: u32::from_le_bytes([src[8], src[9], src[10], src[11]]),
            timestamp_us: u32::from_le_bytes([src[12], src[13], src[14], src[15]]),
        };
        if header.payload_len as usize > MAX_PAYLOAD_LEN {
            return Err(Error::PayloadTooLarge);
        }
        Ok(header)
    }

    /// Serialize this header into the first 16 bytes of `dst`.
    ///
    /// # Panics
    ///
    /// Panics if `dst.len() < HEADER_LEN`.
    pub fn encode(&self, dst: &mut [u8]) {
        assert!(dst.len() >= HEADER_LEN);
        dst[0] = self.magic;
        dst[1] = self.version;
        dst[2] = self.msg_type;
        dst[3] = self.flags.bits();
        dst[4..6].copy_from_slice(&self.channel.to_le_bytes());
        dst[6..8].copy_from_slice(&self.seq.to_le_bytes());
        dst[8..12].copy_from_slice(&self.payload_len.to_le_bytes());
        dst[12..16].copy_from_slice(&self.timestamp_us.to_le_bytes());
    }
}

// ─────────────────────────────────────────────────────────────────────
// Frame helpers
// ─────────────────────────────────────────────────────────────────────

/// Compute the trailing CRC-32C for a pre-packed `header || payload`
/// buffer and write it little-endian into `crc_out[..4]`.
pub fn seal(header_plus_payload: &[u8], crc_out: &mut [u8; CRC_LEN]) {
    let crc = crc32c(header_plus_payload);
    crc_out.copy_from_slice(&crc.to_le_bytes());
}

/// Validate a fully-received frame (`header || payload || CRC`).
/// Returns the parsed header on success.
///
/// # Errors
///
/// Surfaces every variant of [`Error`] depending on which check fails.
pub fn validate(frame: &[u8]) -> Result<Header, Error> {
    if frame.len() < HEADER_LEN + CRC_LEN {
        return Err(Error::FrameTooShort);
    }
    let header = Header::parse(frame)?;
    let expected_len = HEADER_LEN + header.payload_len as usize + CRC_LEN;
    if frame.len() != expected_len {
        return Err(Error::FrameLengthMismatch);
    }
    let body_end = HEADER_LEN + header.payload_len as usize;
    let want = u32::from_le_bytes([
        frame[body_end],
        frame[body_end + 1],
        frame[body_end + 2],
        frame[body_end + 3],
    ]);
    let got = crc32c(&frame[..body_end]);
    if want != got {
        return Err(Error::BadCrc);
    }
    Ok(header)
}

// ─────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_header() -> Header {
        Header {
            magic: MAGIC,
            version: VERSION,
            msg_type: MsgType::Ping as u8,
            flags: Flags::CBOR,
            channel: CHAN_CONTROL,
            seq: 42,
            payload_len: 0,
            timestamp_us: 0x1234_5678,
        }
    }

    #[test]
    fn header_roundtrip() {
        let h = sample_header();
        let mut buf = [0u8; HEADER_LEN];
        h.encode(&mut buf);
        let parsed = Header::parse(&buf).expect("parse");
        assert_eq!(parsed, h);
    }

    #[test]
    fn header_rejects_bad_magic() {
        let mut buf = [0u8; HEADER_LEN];
        sample_header().encode(&mut buf);
        buf[0] = 0x41;
        assert!(matches!(Header::parse(&buf), Err(Error::BadMagicOrVersion)));
    }

    #[test]
    fn header_rejects_reserved_flag_bits() {
        let mut buf = [0u8; HEADER_LEN];
        sample_header().encode(&mut buf);
        buf[3] |= 0b1000_0000;
        assert!(matches!(
            Header::parse(&buf),
            Err(Error::ReservedFlagBits(_))
        ));
    }

    #[test]
    fn header_rejects_oversize_payload() {
        let mut buf = [0u8; HEADER_LEN];
        let mut h = sample_header();
        h.payload_len = u32::try_from(MAX_PAYLOAD_LEN + 1).unwrap();
        h.encode(&mut buf);
        assert!(matches!(Header::parse(&buf), Err(Error::PayloadTooLarge)));
    }

    #[test]
    fn validate_roundtrip_with_payload() {
        let payload = b"hello, rpbridge";
        let mut h = sample_header();
        h.payload_len = u32::try_from(payload.len()).unwrap();
        h.msg_type = MsgType::CmdRequest as u8;

        let mut frame = [0u8; HEADER_LEN + 64];
        h.encode(&mut frame[..HEADER_LEN]);
        frame[HEADER_LEN..HEADER_LEN + payload.len()].copy_from_slice(payload);

        let body_end = HEADER_LEN + payload.len();
        let mut crc_buf = [0u8; CRC_LEN];
        seal(&frame[..body_end], &mut crc_buf);
        frame[body_end..body_end + CRC_LEN].copy_from_slice(&crc_buf);

        let total = body_end + CRC_LEN;
        let parsed = validate(&frame[..total]).expect("validate");
        assert_eq!(parsed.payload_len as usize, payload.len());
        assert_eq!(parsed.msg_type, MsgType::CmdRequest as u8);
    }

    #[test]
    fn validate_detects_crc_flip() {
        let mut h = sample_header();
        h.payload_len = 4;
        let mut frame = [0u8; HEADER_LEN + 4 + CRC_LEN];
        h.encode(&mut frame[..HEADER_LEN]);
        frame[HEADER_LEN..HEADER_LEN + 4].copy_from_slice(&[0xDE, 0xAD, 0xBE, 0xEF]);
        let mut crc_buf = [0u8; CRC_LEN];
        seal(&frame[..HEADER_LEN + 4], &mut crc_buf);
        frame[HEADER_LEN + 4..].copy_from_slice(&crc_buf);
        // flip one bit in the payload → CRC must mismatch
        frame[HEADER_LEN] ^= 0x01;
        assert!(matches!(validate(&frame), Err(Error::BadCrc)));
    }

    #[test]
    fn msg_type_known_values() {
        assert_eq!(MsgType::try_from(0x07).unwrap(), MsgType::Ping);
        assert!(matches!(
            MsgType::try_from(0xEE),
            Err(Error::BadMsgType(0xEE))
        ));
    }
}
