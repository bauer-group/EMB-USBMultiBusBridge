// SPDX-License-Identifier: GPL-2.0
//
// RPBP v1 — framing + CRC-32C, kernel side.
//
// The byte layout mirrors the firmware's `src/rpbp/rpbp.h` one-to-one.
// Both sides are code-reviewed together when the wire format changes;
// there is no schema compiler.

use kernel::prelude::*;

pub const MAGIC: u8 = 0x52; // 'R'
pub const VERSION: u8 = 0x01;
pub const HEADER_LEN: usize = 16;
pub const CRC_LEN: usize = 4;
pub const MAX_PAYLOAD_LEN: usize = 4096;

/// Canonical message-type IDs. Integer values are stable across minor
/// versions of the protocol.
#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum MsgType {
    Hello = 0x00,
    Capabilities = 0x01,
    CmdRequest = 0x02,
    CmdResponse = 0x03,
    StreamData = 0x04,
    StreamCredit = 0x05,
    Event = 0x06,
    Ping = 0x07,
    Pong = 0x08,
    Error = 0x09,
    ResetChannel = 0x0A,
    TimeSync = 0x0B,
}

impl TryFrom<u8> for MsgType {
    type Error = i32;
    fn try_from(v: u8) -> Result<Self, i32> {
        use MsgType::*;
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
            0x09 => Error,
            0x0A => ResetChannel,
            0x0B => TimeSync,
            _ => return Err(crate::errno::EPROTO),
        })
    }
}

bitflags::bitflags! {
    #[derive(Copy, Clone, Debug)]
    pub struct Flags: u8 {
        const CBOR         = 1 << 0;
        const COMPRESSED   = 1 << 1;
        const URGENT       = 1 << 2;
        const FRAGMENT     = 1 << 3;
        const LAST         = 1 << 4;
        const CONTINUATION = 1 << 5;
    }
}

/// Parsed RPBP header. `timestamp_us` is device-boot-relative; the
/// driver translates it to `ktime_get_boottime()` using the TIME_SYNC
/// offset negotiated at attach time.
#[derive(Copy, Clone, Debug)]
pub struct Header {
    pub magic: u8,
    pub version: u8,
    pub msg_type: u8,
    pub flags: Flags,
    pub channel: u16,
    pub seq: u16,
    pub payload_len: u32,
    pub timestamp_us: u32,
}

impl Header {
    pub fn parse(src: &[u8]) -> Result<Self, i32> {
        if src.len() < HEADER_LEN {
            return Err(crate::errno::EPROTO);
        }
        if src[0] != MAGIC || src[1] != VERSION {
            return Err(crate::errno::EPROTO);
        }
        let h = Header {
            magic: src[0],
            version: src[1],
            msg_type: src[2],
            flags: Flags::from_bits_truncate(src[3]),
            channel: u16::from_le_bytes([src[4], src[5]]),
            seq: u16::from_le_bytes([src[6], src[7]]),
            payload_len: u32::from_le_bytes([src[8], src[9], src[10], src[11]]),
            timestamp_us: u32::from_le_bytes([src[12], src[13], src[14], src[15]]),
        };
        if h.payload_len as usize > MAX_PAYLOAD_LEN {
            return Err(crate::errno::EPROTO);
        }
        Ok(h)
    }

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

/// Validate a fully-received RPBP frame (header + payload + CRC).
/// Delegates CRC-32C to the kernel's libcrc32c — same polynomial as the
/// firmware, so the result is bit-identical.
pub fn validate_frame(frame: &[u8]) -> Result<Header, i32> {
    if frame.len() < HEADER_LEN + CRC_LEN {
        return Err(crate::errno::EPROTO);
    }
    let h = Header::parse(frame)?;
    let want_len = HEADER_LEN + h.payload_len as usize + CRC_LEN;
    if frame.len() != want_len {
        return Err(crate::errno::EPROTO);
    }
    let body = &frame[..HEADER_LEN + h.payload_len as usize];
    let crc_on_wire = u32::from_le_bytes([
        frame[HEADER_LEN + h.payload_len as usize],
        frame[HEADER_LEN + h.payload_len as usize + 1],
        frame[HEADER_LEN + h.payload_len as usize + 2],
        frame[HEADER_LEN + h.payload_len as usize + 3],
    ]);
    // libcrc32c inside the kernel implements the same polynomial with
    // the same reflection; see <linux/crc32c.h>.
    //
    // The binding lives under kernel::crc32c — availability depends on
    // the kernel version, so expose a thin wrapper in its own module
    // when bindings land. For now, forward-declare:
    let got = crate::core_driver::crc32c_slice(body);
    if got != crc_on_wire {
        return Err(crate::errno::EPROTO);
    }
    Ok(h)
}
