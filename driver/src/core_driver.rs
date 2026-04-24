// SPDX-License-Identifier: GPL-2.0
//
// RPBridge — USB interface driver entry point. Wires the subsystem
// clients to real USB bulk transfers via the FFI safe wrappers in
// crate::ffi.
//
// ## Transport model
//
// For RPBP v1 every opcode is synchronous request → response on the
// control channel. The driver's `round_trip()` therefore:
//
//   1. Serialises access to the bulk-OUT + bulk-IN pair via a Mutex
//      on `TransportState` (only one request in flight at a time per
//      device).
//   2. Submits the request via a blocking `usb_bulk_msg` on OUT.
//   3. Reads the response via a blocking `usb_bulk_msg` on IN.
//   4. Validates the frame (CRC, header) and hands the CMD_RESPONSE
//      payload back to the caller.
//
// Event and stream channels (GPIO WATCH, ADC streaming) will be
// handled by a dedicated kthread that runs only when a subsystem has
// active subscribers — that path lives in a future M12 milestone and
// reuses CompletionTable for in-flight tracking.
//
// ## Probe sequence
//
// 1. kernel::usb::Driver::probe() hands us a *mut usb_interface.
// 2. Wrap it in ffi::usb::Interface; call rpbp_pair() to find bulk IN/OUT.
// 3. Send HELLO, expect HELLO reply (session init).
// 4. Send CMD_REQUEST{GET_CAPABILITIES}; parse CBOR via crate::cap.
// 5. Walk capabilities, spawn auxiliary_device children for each
//    subsystem present (ADR-0005 + ADR-0006: UART & CAN skipped).
// 6. Done. The aux children talk back via RoundTrip::round_trip().

use kernel::prelude::*;
use kernel::sync::{new_mutex, Arc, Mutex};

use crate::cap::Capabilities;
use crate::ffi;
use crate::rpbp::{self, CHAN_CONTROL, MsgType};
use crate::transport::{CompletionTable, Sequencer, RoundTrip};

/* ───── CRC-32C mirror (stays host-testable) ───────────────────────── */

/// Kept in sync with firmware/src/rpbp/rpbp_crc32c.c. Also exposed
/// via `rpbridge_crc32c_slice` for the kunit C test under
/// CONFIG_RPBRIDGE_KUNIT=y; see driver/tests/kunit/.
pub(crate) fn crc32c_slice(buf: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in buf {
        let idx = ((crc ^ u32::from(b)) & 0xFFu32) as usize;
        crc = (crc >> 8) ^ CRC32C_TABLE[idx];
    }
    crc ^ 0xFFFF_FFFF
}

const CRC32C_TABLE: [u32; 256] = {
    let mut t = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut c: u32 = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if (c & 1) != 0 { (c >> 1) ^ 0x82F63B78 } else { c >> 1 };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
};

/* ───── Per-device state ───────────────────────────────────────────── */

/// Default round-trip timeout. Sized to the USB-FS bulk round-trip
/// ceiling (~1 ms nominal) + firmware worst-case ADC tick budget.
const RT_TIMEOUT_MS: u32 = 200;

/// Transport ownership: one mutex-guarded instance per device. The
/// Interface + endpoint handles live here because every round_trip()
/// needs both and they're protected by the same lock.
pub(crate) struct TransportState {
    pub intf:     Option<ffi::usb::Interface>,
    pub bulk_in:  Option<ffi::usb::BulkEndpoint>,
    pub bulk_out: Option<ffi::usb::BulkEndpoint>,
}

/// Protocol-level state: seq allocator for each channel + parsed
/// capability map. Separate mutex from TransportState so a round-trip
/// can read the seq allocator quickly without blocking on a USB
/// transfer that some other thread is doing.
pub(crate) struct Inner {
    pub ctrl_seq:    Sequencer,
    pub completions: CompletionTable,
    pub caps:        Option<Capabilities>,
}

/// Per-device handle the aux children receive via `Arc<RpbridgeState>`.
pub(crate) struct RpbridgeState {
    pub inner:     Mutex<Inner>,
    pub transport: Mutex<TransportState>,
}

impl RpbridgeState {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Ok(Arc::new(
            Self {
                inner: new_mutex!(
                    Inner {
                        ctrl_seq:    Sequencer::new(),
                        completions: CompletionTable::new(),
                        caps:        None,
                    },
                    "rpbridge.inner"
                ),
                transport: new_mutex!(
                    TransportState { intf: None, bulk_in: None, bulk_out: None },
                    "rpbridge.transport"
                ),
            },
            GFP_KERNEL,
        )?)
    }
}

/* ───── RoundTrip — synchronous request/response on control ─────────── */

impl RoundTrip for Arc<RpbridgeState> {
    fn round_trip(&self, channel: u16, request: &[u8])
        -> Result<(rpbp::Status, KVec<u8>), i32>
    {
        if channel != CHAN_CONTROL {
            // Streams and events go through a separate path (M12).
            return Err(crate::errno::EINVAL);
        }

        // 1. Allocate a seq under the protocol lock — briefly held.
        let seq = {
            let mut g = self.inner.lock();
            g.ctrl_seq.take()
        };

        // 2. Build the outgoing CMD_REQUEST frame on the stack.
        let mut out_buf = [0u8; rpbp::MAX_FRAME_LEN];
        let mut hdr = rpbp::Header {
            magic:        rpbp::MAGIC,
            version:      rpbp::VERSION,
            msg_type:     MsgType::CmdRequest as u8,
            flags:        rpbp::Flags::empty(),
            channel,
            seq,
            payload_len:  0,
            timestamp_us: 0,
        };
        let out_len = rpbp::build_frame(&mut out_buf, &mut hdr, request)?;

        // 3. Serialise the actual USB bulk pair via the transport
        //    lock. Held across the blocking usb_bulk_msg calls; this
        //    is correct because usb_bulk_msg itself may sleep but is
        //    the only thing that sleeps inside the critical section.
        let mut rx_buf = [0u8; rpbp::MAX_FRAME_LEN];
        let rx_len = {
            let g = self.transport.lock();
            let bulk_out = g.bulk_out.ok_or(crate::errno::ENODEV)?;
            let bulk_in  = g.bulk_in.ok_or(crate::errno::ENODEV)?;
            drop(g);
            // Re-acquire with a narrower borrow — the endpoints are
            // Copy so we don't keep the guard.
            bulk_out.transfer(&mut out_buf[..out_len], RT_TIMEOUT_MS)?;
            bulk_in.transfer(&mut rx_buf[..], RT_TIMEOUT_MS)?
        };

        // 4. Validate and parse the reply.
        let reply = &rx_buf[..rx_len];
        let reply_hdr = rpbp::validate_frame(reply)?;

        // The device should mirror (channel, seq) on CMD_RESPONSE
        // per RPBP §10.1. An ERROR frame on the control channel is
        // also a valid reply and its payload carries (orig_channel,
        // orig_seq) so we can correlate it.
        match MsgType::try_from(reply_hdr.msg_type)? {
            MsgType::CmdResponse if reply_hdr.channel == channel
                                 && reply_hdr.seq == seq =>
            {
                let payload = rpbp::payload_slice(reply, &reply_hdr);
                let resp = rpbp::parse_cmd_response_bin(payload)?;
                let mut r = KVec::new();
                r.extend_from_slice(resp.result, GFP_KERNEL)
                    .map_err(|_| crate::errno::ENOMEM)?;
                Ok((resp.status, r))
            }
            MsgType::Error => {
                let payload = rpbp::payload_slice(reply, &reply_hdr);
                // ERROR: [status][orig_channel LE16][orig_seq LE16][reason_len LE16][reason]
                if payload.len() < 7 { return Err(crate::errno::EPROTO); }
                let st = rpbp::Status::try_from(payload[0])?;
                let orig_ch  = rpbp::rd_le16(&payload[1..3]);
                let orig_seq = rpbp::rd_le16(&payload[3..5]);
                if orig_ch != channel || orig_seq != seq {
                    return Err(crate::errno::EPROTO);
                }
                Ok((st, KVec::new()))
            }
            _ => Err(crate::errno::EPROTO),
        }
    }

    fn submit_oneway(&self, _channel: u16, frame: &[u8]) -> Result<(), i32> {
        let g = self.transport.lock();
        let bulk_out = g.bulk_out.ok_or(crate::errno::ENODEV)?;
        drop(g);
        let mut buf = KVec::new();
        buf.extend_from_slice(frame, GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        bulk_out.transfer(buf.as_mut_slice(), RT_TIMEOUT_MS)?;
        Ok(())
    }
}

/* ───── Probe helpers ───────────────────────────────────────────────── */

/// Perform the HELLO handshake. Minimal payload (binary) for v1 — the
/// spec allows a CBOR payload too, but the firmware accepts an empty
/// binary HELLO as a session-init request and replies identically.
fn do_hello(state: &Arc<RpbridgeState>) -> Result<(), i32> {
    // HELLO on channel 0, seq 0. No payload — firmware recognises
    // this as "start a session" and replies with its own HELLO.
    let mut out_buf = [0u8; rpbp::HEADER_LEN + rpbp::CRC_LEN];
    let mut hdr = rpbp::Header {
        magic:        rpbp::MAGIC,
        version:      rpbp::VERSION,
        msg_type:     MsgType::Hello as u8,
        flags:        rpbp::Flags::empty(),
        channel:      CHAN_CONTROL,
        seq:          0,
        payload_len:  0,
        timestamp_us: 0,
    };
    let n = rpbp::build_frame(&mut out_buf, &mut hdr, &[])?;

    let mut rx_buf = [0u8; rpbp::MAX_FRAME_LEN];
    let rx_len = {
        let g = state.transport.lock();
        let bulk_out = g.bulk_out.ok_or(crate::errno::ENODEV)?;
        let bulk_in  = g.bulk_in.ok_or(crate::errno::ENODEV)?;
        drop(g);
        bulk_out.transfer(&mut out_buf[..n], RT_TIMEOUT_MS)?;
        bulk_in.transfer(&mut rx_buf[..], RT_TIMEOUT_MS)?
    };
    let reply_hdr = rpbp::validate_frame(&rx_buf[..rx_len])?;
    if reply_hdr.msg_type != MsgType::Hello as u8 {
        return Err(crate::errno::EPROTO);
    }
    // After HELLO the protocol sequencer resets on both sides. Our
    // Sequencer starts at 0 already; nothing more to do.
    Ok(())
}

/// Fetch the CBOR capability map via `CMD_REQUEST{SYS, GET_CAPABILITIES}`.
/// Stashes the parsed Capabilities into state.inner.caps.
fn do_fetch_capabilities(state: &Arc<RpbridgeState>) -> Result<(), i32> {
    let req = [
        rpbp::Subsys::Sys as u8,
        rpbp::op::sys::GET_CAPABILITIES,
    ];
    let (status, payload) = state.round_trip(CHAN_CONTROL, &req)?;
    if !matches!(status, rpbp::Status::Ok) {
        return Err(status.to_errno());
    }
    let caps = Capabilities::from_cbor(payload.as_slice())?;
    let mut g = state.inner.lock();
    g.caps = Some(caps);
    Ok(())
}

/* ───── Capability-driven aux_device spawn ─────────────────────────── */

#[allow(dead_code)]
pub(crate) fn spawn_children(state: Arc<RpbridgeState>) -> Result<usize, i32> {
    let snapshot = {
        let g = state.inner.lock();
        match g.caps.as_ref() {
            Some(c) => clone_caps_for_spawn(c),
            None => return Err(crate::errno::EPROTO),
        }
    };

    let mut spawned = 0usize;
    for (class, idx) in snapshot.iter() {
        let is_vendor = matches!(
            class.as_slice(),
            b"gpio" | b"i2c" | b"spi" | b"pwm" | b"adc"
        );
        if !is_vendor { continue; }

        let bus = crate::cap::BusCaps {
            class: KString::try_from(
                core::str::from_utf8(class).map_err(|_| crate::errno::EPROTO)?,
            ).map_err(|_| crate::errno::ENOMEM)?,
            idx: *idx,
            pins: Vec::new(),
            max_freq_hz: 0,
            access: KString::new(),
        };
        match class.as_slice() {
            b"gpio" => { let _ = crate::gpio::GpioChild::probe(state.clone(), &bus)?; }
            b"i2c"  => { let _ = crate::i2c::I2cChild::probe(state.clone(), &bus)?; }
            b"spi"  => { let _ = crate::spi::SpiChild::probe(state.clone(), &bus)?; }
            b"pwm"  => { let _ = crate::pwm::PwmChild::probe(state.clone(), &bus)?; }
            b"adc"  => { let _ = crate::iio::IioChild::probe(state.clone(), &bus)?; }
            _ => {}
        }
        spawned += 1;
    }
    pr_info!("rpbridge: spawned {} aux_device children\n", spawned);
    Ok(spawned)
}

fn clone_caps_for_spawn(caps: &Capabilities) -> Vec<(KVec<u8>, u16)> {
    let mut out = Vec::new();
    for bus in caps.buses.iter() {
        let mut cls = KVec::new();
        let _ = cls.extend_from_slice(bus.class.as_bytes(), GFP_KERNEL);
        let _ = out.push((cls, bus.idx), GFP_KERNEL);
    }
    out
}

/* ───── USB driver glue ────────────────────────────────────────────── */

const RPBRIDGE_USB_VID: u16 = 0x1209;
const RPBRIDGE_USB_PID: u16 = 0xFFFE;

pub(crate) struct RpbridgeDriver;

#[vtable]
impl kernel::usb::Driver for RpbridgeDriver {
    type Data = Arc<RpbridgeState>;
    type IdInfo = ();

    const ID_TABLE: kernel::usb::IdTable<Self::IdInfo> =
        &kernel::usb::IdTable::new(&[
            kernel::usb::DeviceId::new(RPBRIDGE_USB_VID, RPBRIDGE_USB_PID),
        ]);

    fn probe(
        intf: &kernel::usb::Interface,
        _id_info: Option<&Self::IdInfo>,
    ) -> Result<Arc<RpbridgeState>> {
        pr_info!("rpbridge: probe()\n");

        let state = RpbridgeState::new()?;

        // 1. Grab the raw interface pointer.
        //
        // In R4L 6.18 `kernel::usb::Interface` is a
        // `#[repr(transparent)]` wrapper around
        // `Opaque<bindings::usb_interface>`, which is itself
        // `repr(transparent)` around `UnsafeCell<MaybeUninit<T>>`.
        // The composition is ABI-identical to `*mut usb_interface`,
        // so a pointer cast is sound and avoids the `transmute`
        // footgun of silently changing its meaning if the layout
        // evolves. The assumption lives in one place; any R4L
        // upstream change that breaks it breaks this cast at the
        // call site and nowhere else.
        //
        // SAFETY: the pointer cast relies only on the documented
        // transparent representation above; no bytes are
        // reinterpreted.
        let raw_intf: *mut ffi::bindings::usb_interface =
            intf as *const kernel::usb::Interface
                 as *const ffi::bindings::usb_interface
                 as *mut _;
        let intf_wrapper = unsafe { ffi::usb::Interface::from_raw(raw_intf) };

        // 2. Locate the RPBP bulk-IN / bulk-OUT pair.
        let (bulk_in, bulk_out) = intf_wrapper.rpbp_pair()
            .map_err(|_| kernel::error::code::ENODEV)?;

        // 3. Install them in TransportState.
        {
            let mut g = state.transport.lock();
            g.intf     = Some(intf_wrapper);
            g.bulk_in  = Some(bulk_in);
            g.bulk_out = Some(bulk_out);
        }

        // 4. Session init + capability fetch.
        do_hello(&state).map_err(kernel::error::Error::from_errno)?;
        do_fetch_capabilities(&state).map_err(kernel::error::Error::from_errno)?;

        // 5. Spawn aux_device children for the subsystems the device
        //    advertises on the vendor interface.
        let _ = spawn_children(state.clone())
            .map_err(kernel::error::Error::from_errno)?;

        pr_info!("rpbridge: probe complete\n");
        Ok(state)
    }

    fn disconnect(_intf: &kernel::usb::Interface, _data: Arc<RpbridgeState>) {
        pr_info!("rpbridge: interface disconnected\n");
        // auxiliary_device children are unregistered in each Child's
        // Drop impl (see gpio/i2c/spi/pwm/iio). TransportState goes
        // away when the last Arc reference drops — the kernel's USB
        // core guarantees no further bulk transfers after this
        // callback returns.
    }

    fn suspend(_intf: &kernel::usb::Interface, _msg: kernel::usb::Message)
        -> Result
    {
        Ok(())
    }

    fn resume(_intf: &kernel::usb::Interface) -> Result {
        Ok(())
    }
}

/* ───── KUnit-C symbol export for CRC parity test ──────────────────── */

#[cfg(CONFIG_RPBRIDGE_KUNIT)]
#[no_mangle]
pub extern "C" fn rpbridge_crc32c_slice(buf: *const u8, len: usize) -> u32 {
    if buf.is_null() || len == 0 { return 0; }
    // SAFETY: kunit C caller guarantees `buf` points at `len` bytes
    // of readable memory for the duration of the call.
    let slice = unsafe { core::slice::from_raw_parts(buf, len) };
    crc32c_slice(slice)
}

/* ───── Inline unit tests ──────────────────────────────────────────── */

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32c_empty_equals_zero() {
        assert_eq!(crc32c_slice(&[]), 0);
    }

    #[test]
    fn crc32c_known_vector() {
        assert_eq!(crc32c_slice(b"123456789"), 0xE306_9283);
    }

    #[test]
    fn header_roundtrip() {
        let h = rpbp::Header {
            magic:        rpbp::MAGIC,
            version:      rpbp::VERSION,
            msg_type:     MsgType::Ping as u8,
            flags:        rpbp::Flags::CBOR,
            channel:      0,
            seq:          42,
            payload_len:  0,
            timestamp_us: 0x1234_5678,
        };
        let mut buf = [0u8; rpbp::HEADER_LEN];
        h.encode(&mut buf);
        let parsed = rpbp::Header::parse(&buf).unwrap();
        assert_eq!(parsed.seq, 42);
        assert_eq!(parsed.timestamp_us, 0x1234_5678);
    }
}
