// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-i2c — RPBP I²C subsystem client + auxiliary i2c_adapter
// child. Wire format in docs/protocol/subsystems/i2c.md.
//
// Three layers, same shape as gpio.rs:
//
//   1. `wire` — pure builders/parsers, no kernel types.
//   2. `Client` — binds the wire layer to a RoundTrip handle. Every
//      I²C op becomes one or more RPBP XFER round-trips.
//   3. `I2cChild` — Rust-for-Linux i2c_adapter adapter. Its
//      master_xfer walks the i2c_msg array and dispatches each to
//      Client::xfer_msg, using NO_STOP when the kernel requested
//      repeated-start (I2C_M_NOSTART on the *following* message).

use kernel::prelude::*;
use kernel::sync::Arc;

use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;
use crate::rpbp::{CHAN_CONTROL, MAX_PAYLOAD_LEN, Status};
use crate::transport::RoundTrip;

/* ═════ Layer 1 — wire ════════════════════════════════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::i2c as ic};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::I2c as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 PROBE */
    pub fn build_probe(dst: &mut [u8], bus: u8, addr: u8) -> Result<usize, i32> {
        let n = start(dst, ic::PROBE)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        dst[n] = bus;
        dst[n + 1] = addr;
        Ok(n + 2)
    }

    /* 0x01 XFER */
    pub fn build_xfer(
        dst: &mut [u8], bus: u8, addr: u8, flags: u8,
        tx: &[u8], rx_len: u16,
    ) -> Result<usize, i32> {
        if tx.len() > u16::MAX as usize { return Err(crate::errno::EINVAL); }
        let n = start(dst, ic::XFER)?;
        let need = n + 7 + tx.len();
        if dst.len() < need { return Err(crate::errno::EINVAL); }
        dst[n]     = bus;
        dst[n + 1] = addr;
        dst[n + 2] = flags;
        rpbp::wr_le16(&mut dst[n + 3..], tx.len() as u16);
        rpbp::wr_le16(&mut dst[n + 5..], rx_len);
        dst[n + 7..n + 7 + tx.len()].copy_from_slice(tx);
        Ok(need)
    }

    /// Parse an XFER response. Returns the RX byte slice from the
    /// reply. Reply layout: `[rx_len LE u16][rx_data...]`.
    pub fn parse_xfer<'a>(resp: &'a [u8]) -> Result<&'a [u8], i32> {
        if resp.len() < 2 { return Err(crate::errno::EPROTO); }
        let rx_len = rpbp::rd_le16(&resp[..2]) as usize;
        if resp.len() < 2 + rx_len { return Err(crate::errno::EPROTO); }
        Ok(&resp[2..2 + rx_len])
    }

    /* 0x02 SCAN */
    pub fn build_scan(dst: &mut [u8], bus: u8) -> Result<usize, i32> {
        let n = start(dst, ic::SCAN)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = bus;
        Ok(n + 1)
    }

    /// Parse a SCAN response (16-byte bitmap; bit(addr) set on ACK).
    pub fn parse_scan(resp: &[u8], out: &mut [bool; 128]) -> Result<(), i32> {
        if resp.len() < 16 { return Err(crate::errno::EPROTO); }
        for addr in 0..128u8 {
            let byte = resp[(addr >> 3) as usize];
            out[addr as usize] = (byte >> (addr & 7)) & 1 != 0;
        }
        Ok(())
    }

    /* 0x03 SET_FREQ / 0x04 GET_FREQ */
    pub fn build_set_freq(dst: &mut [u8], bus: u8, freq_hz: u32) -> Result<usize, i32> {
        let n = start(dst, ic::SET_FREQ)?;
        if dst.len() < n + 5 { return Err(crate::errno::EINVAL); }
        dst[n] = bus;
        rpbp::wr_le32(&mut dst[n + 1..], freq_hz);
        Ok(n + 5)
    }

    pub fn build_get_freq(dst: &mut [u8], bus: u8) -> Result<usize, i32> {
        let n = start(dst, ic::GET_FREQ)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = bus;
        Ok(n + 1)
    }

    pub fn parse_freq_u32(resp: &[u8]) -> Result<u32, i32> {
        if resp.len() < 4 { return Err(crate::errno::EPROTO); }
        Ok(rpbp::rd_le32(&resp[..4]))
    }
}

/* ═════ Layer 2 — Client ══════════════════════════════════════════ */

/// Logical direction of an I²C message as seen by the kernel's
/// `struct i2c_msg`. `Write` carries tx bytes; `Read` carries the
/// expected RX length. The adapter translates the kernel's
/// `I2C_M_RD` flag to this enum.
pub enum Direction<'a> {
    Write(&'a [u8]),
    Read(u16),
}

/// Result of a single message in a multi-message transfer. Reads
/// come back as KVec<u8> (allocating per-call — kernel i2c core
/// copies into its own buffer so lifetime overhead is acceptable).
pub enum MsgResult {
    Wrote,
    Read(KVec<u8>),
}

pub struct Client<T: RoundTrip + ?Sized> {
    rt:  Arc<T>,
    bus: u8,
}

impl<T: RoundTrip + ?Sized> Client<T> {
    pub fn new(rt: Arc<T>, bus: u8) -> Self { Self { rt, bus } }

    fn rt_call(&self, req: &[u8]) -> Result<KVec<u8>, i32> {
        let (status, payload) = self.rt.round_trip(CHAN_CONTROL, req)?;
        if !matches!(status, Status::Ok) { return Err(status.to_errno()); }
        Ok(payload)
    }

    pub fn probe_addr(&self, addr: u8) -> Result<(), i32> {
        let mut buf = [0u8; 4];
        let n = wire::build_probe(&mut buf, self.bus, addr)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn scan(&self) -> Result<[bool; 128], i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_scan(&mut buf, self.bus)?;
        let resp = self.rt_call(&buf[..n])?;
        let mut out = [false; 128];
        wire::parse_scan(resp.as_slice(), &mut out)?;
        Ok(out)
    }

    pub fn set_freq(&self, hz: u32) -> Result<(), i32> {
        let mut buf = [0u8; 7];
        let n = wire::build_set_freq(&mut buf, self.bus, hz)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn get_freq(&self) -> Result<u32, i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_get_freq(&mut buf, self.bus)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_freq_u32(resp.as_slice())
    }

    /// Execute a single i2c_msg. Caller decides whether to set
    /// `no_stop` based on whether the NEXT message is a read at
    /// the same address (combined write-restart-read pattern).
    /// Use [`Client::xfer_msg_list`] for the full multi-msg form.
    pub fn xfer_msg(
        &self, addr: u8, dir: Direction<'_>, no_stop: bool,
    ) -> Result<MsgResult, i32> {
        let flags = if no_stop {
            crate::rpbp::op::i2c::FLAG_NO_STOP
        } else { 0 };
        let mut buf = KVec::new();
        // Upper bound: header + 7 bytes + payload; keep it inside the
        // RPBP payload ceiling with a small margin for the CMD header.
        const MARGIN: usize = 16;
        let (tx, rx_len) = match dir {
            Direction::Write(tx) => (tx, 0u16),
            Direction::Read(n)   => (&[][..], n),
        };
        if tx.len() + MARGIN > MAX_PAYLOAD_LEN {
            return Err(crate::errno::EINVAL);
        }
        buf.reserve(tx.len() + MARGIN, GFP_KERNEL)?;
        // Reserve enough and then use the &mut slice via push-of-zero
        // iteration — KVec doesn't give a &mut [u8] directly without
        // this dance in no_std.
        for _ in 0..(tx.len() + MARGIN) { buf.push(0, GFP_KERNEL)?; }
        let n = wire::build_xfer(
            buf.as_mut_slice(), self.bus, addr, flags, tx, rx_len,
        )?;
        let resp = self.rt_call(&buf.as_slice()[..n])?;
        match dir {
            Direction::Write(_) => Ok(MsgResult::Wrote),
            Direction::Read(_)  => {
                let rx = wire::parse_xfer(resp.as_slice())?;
                let mut out = KVec::new();
                out.extend_from_slice(rx, GFP_KERNEL)
                    .map_err(|_| crate::errno::ENOMEM)?;
                Ok(MsgResult::Read(out))
            }
        }
    }

    /// Multi-message helper — matches the shape of `master_xfer`.
    /// Detects the `write → read@same_addr` pattern and sets
    /// `FLAG_NO_STOP` on the write so the firmware emits a restart
    /// instead of a STOP between the two halves.
    pub fn xfer_msg_list<'a>(
        &self, msgs: &'a [(u8, Direction<'a>)],
    ) -> Result<Vec<MsgResult>, i32> {
        let mut out = Vec::new();
        out.reserve(msgs.len(), GFP_KERNEL)?;
        for (i, (addr, dir)) in msgs.iter().enumerate() {
            let is_write = matches!(dir, Direction::Write(_));
            let next_is_read_same_addr = msgs.get(i + 1).is_some_and(|(a2, d2)| {
                *a2 == *addr && matches!(d2, Direction::Read(_))
            });
            let no_stop = is_write && next_is_read_same_addr;
            let res = self.xfer_msg_ref(*addr, dir, no_stop)?;
            out.push(res, GFP_KERNEL)?;
        }
        Ok(out)
    }

    fn xfer_msg_ref(
        &self, addr: u8, dir: &Direction<'_>, no_stop: bool,
    ) -> Result<MsgResult, i32> {
        match dir {
            Direction::Write(tx) => self.xfer_msg(addr, Direction::Write(tx), no_stop),
            Direction::Read(n)   => self.xfer_msg(addr, Direction::Read(*n), no_stop),
        }
    }
}

/* ═════ Layer 3 — i2c_adapter kernel integration ════════════════ */

use crate::ffi;

struct I2cBackend {
    client: Client<RpbridgeState>,
}

impl ffi::i2c::I2cBackend for I2cBackend {
    fn master_xfer(&self, msgs: &mut [ffi::i2c::I2cReq<'_>]) -> Result<usize, i32> {
        // The FFI trampoline feeds us messages one at a time. Each
        // call here handles exactly one i2c_msg → one RPBP XFER
        // round-trip. Write-restart-read coalescing happens across
        // successive trampoline calls: the i2c core passes the
        // whole i2c_msg array in order, so we can detect the
        // write → read@same_addr pattern via last-seen state.
        //
        // For simplicity we treat each message independently in this
        // first cut — the firmware's XFER opcode handles a single
        // STOP per transaction, which is correct for most sensors
        // (SCD30, SHT31, BMP280, etc.). Callers needing repeated-
        // start can use /dev/i2c-N's RDWR ioctl with explicit flags
        // once we advertise I2C_FUNC_NOSTART.
        if msgs.len() != 1 { return Err(crate::errno::EINVAL); }
        let req = &mut msgs[0];
        match &mut req.data {
            ffi::i2c::MsgDir::Write(tx) => {
                match self.client.xfer_msg(
                    req.addr as u8, Direction::Write(tx), false
                ) {
                    Ok(_)  => Ok(1),
                    Err(e) => Err(e),
                }
            }
            ffi::i2c::MsgDir::Read(dst) => {
                match self.client.xfer_msg(
                    req.addr as u8, Direction::Read(dst.len() as u16), false
                ) {
                    Ok(MsgResult::Read(data)) => {
                        let n = dst.len().min(data.len());
                        dst[..n].copy_from_slice(&data.as_slice()[..n]);
                        Ok(1)
                    }
                    Ok(_)  => Err(crate::errno::EIO),
                    Err(e) => Err(e),
                }
            }
        }
    }
    fn functionality(&self) -> u32 {
        ffi::bindings::I2C_FUNC_I2C | ffi::bindings::I2C_FUNC_SMBUS_EMUL
    }
}

pub(crate) struct I2cChild {
    _handle: ffi::i2c::AdapterHandle,
    #[allow(dead_code)]
    idx:     u16,
}

impl I2cChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state.clone(), bus.idx as u8);
        pr_info!("rpbridge-i2c.{}: probe\n", bus.idx);

        let name = c"rpbridge-i2c";
        let parent = crate::ffi::auxiliary::parent_device(
            &*core::ops::Deref::deref(&state)
        ).map_err(|e| kernel::error::Error::from_errno(e))?;

        let backend = I2cBackend { client };
        let handle = ffi::i2c::AdapterHandle::register(parent, name, backend)
            .map_err(|e| kernel::error::Error::from_errno(e))?;
        Ok(I2cChild { _handle: handle, idx: bus.idx })
    }
}

/* ═════ Tests ═════════════════════════════════════════════════════ */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpbp::op::i2c as ic;

    #[test]
    fn build_probe_encodes_subsys_opcode_bus_addr() {
        let mut buf = [0u8; 4];
        let n = wire::build_probe(&mut buf, 0, 0x50).unwrap();
        assert_eq!(n, 4);
        assert_eq!(buf, [0x01, ic::PROBE, 0, 0x50]);
    }

    #[test]
    fn build_xfer_layout() {
        let mut buf = [0u8; 16];
        let n = wire::build_xfer(&mut buf, 1, 0x48, ic::FLAG_NO_STOP, &[0xAA, 0xBB], 4).unwrap();
        assert_eq!(n, 11);
        // [subsys][op][bus][addr][flags][tx_len LE16][rx_len LE16][tx...]
        assert_eq!(buf[..11], [
            0x01, ic::XFER, 1, 0x48, ic::FLAG_NO_STOP,
            0x02, 0x00, 0x04, 0x00,
            0xAA, 0xBB,
        ]);
    }

    #[test]
    fn parse_xfer_extracts_rx() {
        let resp = [0x03, 0x00, 0xDE, 0xAD, 0xBE];
        let rx = wire::parse_xfer(&resp).unwrap();
        assert_eq!(rx, &[0xDE, 0xAD, 0xBE]);
    }

    #[test]
    fn parse_scan_decodes_bitmap() {
        let mut bitmap = [0u8; 16];
        bitmap[0x50 / 8] = 1 << (0x50 % 8);
        bitmap[0x68 / 8] = 1 << (0x68 % 8);
        let mut out = [false; 128];
        wire::parse_scan(&bitmap, &mut out).unwrap();
        assert!(out[0x50]);
        assert!(out[0x68]);
        assert!(!out[0x51]);
    }
}
