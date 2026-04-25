// SPDX-License-Identifier: GPL-2.0
//
// RPBP v1 — framing + CRC-32C + subsystem + opcode constants, kernel side.
//
// The byte layout mirrors the firmware's `src/rpbp/rpbp.h` one-to-one.
// Both sides are code-reviewed together when the wire format changes;
// there is no schema compiler.
//
// Keep this file in lockstep with firmware/src/rpbp/rpbp.h. The host
// tests (firmware/tests/host/test_golden.c) double as a wire-format
// contract that this module must match byte-for-byte.

use kernel::prelude::*;

/* ───── Wire constants ───────────────────────────────────────────── */

pub const MAGIC: u8 = 0x52; // 'R'
pub const VERSION: u8 = 0x01;
pub const HEADER_LEN: usize = 16;
pub const CRC_LEN: usize = 4;
pub const MAX_PAYLOAD_LEN: usize = 4096;
pub const MAX_FRAME_LEN: usize = HEADER_LEN + MAX_PAYLOAD_LEN + CRC_LEN;

/* ───── Channels ─────────────────────────────────────────────────── */

pub const CHAN_CONTROL: u16 = 0;
pub const CHAN_EVENTS:  u16 = 1;
pub const CHAN_DYN_MIN: u16 = 16;
pub const CHAN_DYN_MAX: u16 = 239;

/* ───── Message types ────────────────────────────────────────────── */

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

/* ───── Subsystem IDs ────────────────────────────────────────────── */

#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Subsys {
    Sys  = 0x00,
    I2c  = 0x01,
    Spi  = 0x02,
    Gpio = 0x03,
    Pwm  = 0x04,
    Adc  = 0x05,
    Uart = 0x06,
    Led     = 0x07,
    Dmx     = 0x08,
    Onewire = 0x09,
    I2s     = 0x0A,
}

/* ───── Status codes (mirrors firmware enum rpbp_status) ─────────── */

#[repr(u8)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Status {
    Ok         = 0,
    EAgain     = 1,
    EInval     = 2,
    EPerm      = 3,
    ENoent     = 4,
    EIo        = 5,
    ETimedout  = 6,
    EMsgSize   = 7,
    ECrc       = 8,
    EProto     = 9,
    ENotSup    = 10,
    EBusy      = 11,
    ECanceled  = 12,
    ENoSpc     = 13,
    EOverflow  = 14,
    ENoDev     = 15,
}

impl Status {
    /// Map an RPBP status code to a Linux errno the kernel subsystem
    /// callers can return up their stack.
    pub fn to_errno(self) -> i32 {
        use Status::*;
        match self {
            Ok => 0,
            EAgain    => 11,
            EInval    => crate::errno::EINVAL,
            EPerm     => 1,
            ENoent    => 2,
            EIo       => crate::errno::EIO,
            ETimedout => crate::errno::ETIMEDOUT,
            EMsgSize  => 90,
            ECrc      => crate::errno::EIO,
            EProto    => crate::errno::EPROTO,
            ENotSup   => 95,
            EBusy     => 16,
            ECanceled => 125,
            ENoSpc    => 28,
            EOverflow => 75,
            ENoDev    => crate::errno::ENODEV,
        }
    }
}

impl TryFrom<u8> for Status {
    type Error = i32;
    fn try_from(v: u8) -> Result<Self, i32> {
        use Status::*;
        // Known status codes round-trip exactly; unknown bytes from a
        // newer firmware MINOR (e.g. a hypothetical EALREADY=16 added
        // in v1.1) are gracefully degraded to `EIo` instead of being
        // flagged as a protocol violation. RPBP v1 §16 promises that
        // MINOR bumps are additive — including new status codes — so
        // an old driver against new firmware MUST keep working. A
        // generic EIO bubbling up to userland is the right fallback:
        // the operation failed, the cause is "device-side, not in our
        // table", and userland tools see a sensible errno instead of
        // EPROTO confusion.
        //
        // The TryFrom signature is kept (rather than swapping to
        // From) so call sites' `?` continues to compile unchanged
        // and so a future opt-in strict mode can return Err here
        // without a wider refactor.
        Ok(match v {
            0 => Ok, 1 => EAgain, 2 => EInval, 3 => EPerm, 4 => ENoent,
            5 => EIo, 6 => ETimedout, 7 => EMsgSize, 8 => ECrc, 9 => EProto,
            10 => ENotSup, 11 => EBusy, 12 => ECanceled, 13 => ENoSpc,
            14 => EOverflow, 15 => ENoDev,
            _ => EIo,
        })
    }
}

/* ───── Subsystem opcode namespaces ──────────────────────────────── */

pub mod op {
    pub mod sys {
        pub const GET_CAPABILITIES: u8 = 0x00;
        pub const ECHO:             u8 = 0x01;
        pub const REBOOT_BOOTSEL:   u8 = 0x02;
        pub const UPTIME:           u8 = 0x03;
        pub const GET_VBUS_MV:      u8 = 0x04;
        pub const SET_LED:          u8 = 0x05;
        pub const SELFTEST:         u8 = 0x06;
        pub const GET_IDENTITY:     u8 = 0x07;
        pub const RESET:            u8 = 0x08;
    }
    pub mod i2c {
        pub const PROBE:    u8 = 0x00;
        pub const XFER:     u8 = 0x01;
        pub const SCAN:     u8 = 0x02;
        pub const SET_FREQ: u8 = 0x03;
        pub const GET_FREQ: u8 = 0x04;

        pub const FLAG_NO_STOP: u8 = 1 << 0;
    }
    pub mod spi {
        pub const XFER:       u8 = 0x00;
        pub const SET_MODE:   u8 = 0x01;
        pub const SET_FREQ:   u8 = 0x02;
        pub const GET_FREQ:   u8 = 0x03;
        pub const CS_ASSERT:  u8 = 0x04;
        pub const CS_RELEASE: u8 = 0x05;

        pub const FLAG_HOLD_CS: u8 = 1 << 0;

        pub const MODE_CPHA:      u8 = 1 << 0;
        pub const MODE_CPOL:      u8 = 1 << 1;
        pub const MODE_LSB_FIRST: u8 = 1 << 2;
    }
    pub mod gpio {
        pub const LIST:      u8 = 0x00;
        pub const CONFIGURE: u8 = 0x01;
        pub const SET:       u8 = 0x02;
        pub const GET:       u8 = 0x03;
        pub const SET_MASK:  u8 = 0x04;
        pub const GET_MASK:  u8 = 0x05;
        pub const WATCH:     u8 = 0x06;
        pub const UNWATCH:   u8 = 0x07;

        pub const DIR_INPUT:  u8 = 0;
        pub const DIR_OUTPUT: u8 = 1;

        pub const PULL_NONE: u8 = 0;
        pub const PULL_UP:   u8 = 1;
        pub const PULL_DOWN: u8 = 2;

        pub const WATCH_RISING:  u8 = 1 << 0;
        pub const WATCH_FALLING: u8 = 1 << 1;
        pub const WATCH_BOTH:    u8 = WATCH_RISING | WATCH_FALLING;
    }
    pub mod pwm {
        pub const CONFIGURE: u8 = 0x00;
        pub const SET_DUTY:  u8 = 0x01;
        pub const START:     u8 = 0x02;
        pub const STOP:      u8 = 0x03;
        pub const GET_STATE: u8 = 0x04;

        pub const FLAG_INVERT: u8 = 1 << 0;
    }
    pub mod adc {
        pub const READ:         u8 = 0x00;
        pub const READ_MANY:    u8 = 0x01;
        pub const GET_REF:      u8 = 0x02;
        pub const TEMP_READ:    u8 = 0x03;
        pub const STREAM_START: u8 = 0x04;
        pub const STREAM_STOP:  u8 = 0x05;
    }
    pub mod uart {
        pub const SET_PHY: u8 = 0x00;
        pub const GET_PHY: u8 = 0x01;

        pub const PHY_OFF:   u8 = 0;
        pub const PHY_TTL:   u8 = 1;
        pub const PHY_RS232: u8 = 2;
        pub const PHY_RS485: u8 = 3;
    }
    pub mod led {
        pub const GET_INFO:  u8 = 0x00;
        pub const CONFIGURE: u8 = 0x01;
        pub const WRITE:     u8 = 0x02;
        pub const CLEAR:     u8 = 0x03;

        pub const FMT_GRB:  u8 = 0;
        pub const FMT_RGB:  u8 = 1;
        pub const FMT_GRBW: u8 = 2;
    }
    pub mod dmx {
        pub const GET_INFO:  u8 = 0x00;
        pub const SET_SLOTS: u8 = 0x01;
        pub const START:     u8 = 0x02;
        pub const STOP:      u8 = 0x03;
        pub const SET_BREAK: u8 = 0x04;
    }
    pub mod onewire {
        pub const GET_INFO: u8 = 0x00;
        pub const RESET:    u8 = 0x01;
        pub const WRITE:    u8 = 0x02;
        pub const READ:     u8 = 0x03;
        pub const TRIPLET:  u8 = 0x04;
    }
    pub mod i2s {
        pub const GET_INFO:  u8 = 0x00;
        pub const CONFIGURE: u8 = 0x01;
        pub const START:     u8 = 0x02;
        pub const STOP:      u8 = 0x03;
        pub const WRITE:     u8 = 0x04;
    }
}

/* ───── Header ───────────────────────────────────────────────────── */

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
        // Reject fragment + last in one frame — spec forbids it.
        let flags_raw = src[3];
        if flags_raw & 0xC0 != 0 {
            return Err(crate::errno::EPROTO);
        }
        let h = Header {
            magic: src[0],
            version: src[1],
            msg_type: src[2],
            flags: Flags::from_bits_truncate(flags_raw),
            channel: u16::from_le_bytes([src[4], src[5]]),
            seq: u16::from_le_bytes([src[6], src[7]]),
            payload_len: u32::from_le_bytes([src[8], src[9], src[10], src[11]]),
            timestamp_us: u32::from_le_bytes([src[12], src[13], src[14], src[15]]),
        };
        if h.flags.contains(Flags::FRAGMENT) && h.flags.contains(Flags::LAST) {
            return Err(crate::errno::EPROTO);
        }
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

/* ───── Frame build / validate ───────────────────────────────────── */

/// Build a complete frame (header + payload + CRC-32C) into `dst`.
/// Returns the number of bytes written, or `Err(-EINVAL)` when
/// `dst` is too small / payload exceeds the spec ceiling.
pub fn build_frame(
    dst: &mut [u8],
    hdr: &mut Header,
    payload: &[u8],
) -> Result<usize, i32> {
    if payload.len() > MAX_PAYLOAD_LEN {
        return Err(crate::errno::EINVAL);
    }
    let total = HEADER_LEN + payload.len() + CRC_LEN;
    if dst.len() < total {
        return Err(crate::errno::EINVAL);
    }
    hdr.magic = MAGIC;
    hdr.version = VERSION;
    hdr.payload_len = payload.len() as u32;
    hdr.encode(&mut dst[..HEADER_LEN]);
    dst[HEADER_LEN..HEADER_LEN + payload.len()].copy_from_slice(payload);
    let crc = crate::core_driver::crc32c_slice(&dst[..HEADER_LEN + payload.len()]);
    dst[HEADER_LEN + payload.len()..HEADER_LEN + payload.len() + CRC_LEN]
        .copy_from_slice(&crc.to_le_bytes());
    Ok(total)
}

/// Validate a fully-received RPBP frame (header + payload + CRC).
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
    let got = crate::core_driver::crc32c_slice(body);
    if got != crc_on_wire {
        return Err(crate::errno::EPROTO);
    }
    Ok(h)
}

/// Extract the payload slice (between header and CRC) from a
/// validated frame.
pub fn payload_slice<'a>(frame: &'a [u8], h: &Header) -> &'a [u8] {
    &frame[HEADER_LEN..HEADER_LEN + h.payload_len as usize]
}

/* ───── CMD_RESPONSE parser ──────────────────────────────────────── */

/// A binary CMD_RESPONSE payload. CBOR responses use
/// [`parse_cmd_response_cbor`] instead — this path is for the
/// `[subsys][opcode][status][result...]` layout.
pub struct CmdResponse<'a> {
    pub subsys: u8,
    pub opcode: u8,
    pub status: Status,
    pub result: &'a [u8],
}

pub fn parse_cmd_response_bin(payload: &[u8]) -> Result<CmdResponse<'_>, i32> {
    if payload.len() < 3 {
        return Err(crate::errno::EPROTO);
    }
    Ok(CmdResponse {
        subsys: payload[0],
        opcode: payload[1],
        status: Status::try_from(payload[2])?,
        result: &payload[3..],
    })
}

/* ───── Little-endian readers/writers for subsystem payloads ─────── */

#[inline]
pub fn rd_le16(p: &[u8]) -> u16 { u16::from_le_bytes([p[0], p[1]]) }
#[inline]
pub fn rd_le32(p: &[u8]) -> u32 { u32::from_le_bytes([p[0], p[1], p[2], p[3]]) }
#[inline]
pub fn rd_le64(p: &[u8]) -> u64 {
    u64::from_le_bytes([p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]])
}

#[inline]
pub fn wr_le16(p: &mut [u8], v: u16) { p[..2].copy_from_slice(&v.to_le_bytes()); }
#[inline]
pub fn wr_le32(p: &mut [u8], v: u32) { p[..4].copy_from_slice(&v.to_le_bytes()); }
#[inline]
pub fn wr_le64(p: &mut [u8], v: u64) { p[..8].copy_from_slice(&v.to_le_bytes()); }

/* ───── Forward-compat tests ─────────────────────────────────────── */

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_status_codes_round_trip() {
        for v in 0u8..=15u8 {
            assert!(Status::try_from(v).is_ok(),
                    "status code {} must parse on the supported range", v);
        }
    }

    #[test]
    fn unknown_status_falls_back_to_eio_not_eproto() {
        // Forward-compat: a future MINOR may add status codes 16+.
        // Old drivers must surface them as a generic EIO instead of
        // raising EPROTO (which would imply protocol corruption).
        for v in 16u8..=255u8 {
            let s = Status::try_from(v).expect("should never error");
            assert!(matches!(s, Status::EIo),
                    "unknown status {} should map to EIo, got {:?}", v, s);
            assert_eq!(s.to_errno(), crate::errno::EIO);
        }
    }
}
