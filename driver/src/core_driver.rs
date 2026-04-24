// SPDX-License-Identifier: GPL-2.0
//
// RPBridge — USB interface driver entry point.
//
// Lifecycle:
//
//   probe()     USB interface matched. Allocate per-device state, submit
//               the first bulk-IN URB, send HELLO + GET_CAPABILITIES,
//               parse the CBOR capability map, then spawn one
//               auxiliary_device per advertised subsystem.
//
//   disconnect() Tear down in reverse: unregister auxiliary_devices,
//                kill anchored URBs, drop per-device state. The kernel
//                USB core guarantees no further callbacks once
//                usb_kill_anchored_urbs() has returned.
//
// Most of the plumbing lives in subordinate modules:
//   * `crate::rpbp`      — RPBP v1 framing + CRC-32C + subsystem constants
//   * `crate::cap`       — CBOR capability-map parser
//   * `crate::transport` — sequence allocator + completion table
//   * `crate::{gpio,i2c,spi,pwm,iio}` — per-subsystem wire helpers +
//     auxiliary_device children
//
// The USB-facing code — URB submission, endpoint discovery,
// auxiliary_device_register — is thin by design. Where Rust-for-Linux
// bindings are not yet available in the target kernel, the code is
// structured so the missing call is a single line of pseudo-Rust next
// to a TODO that names the kernel API it will call.

use kernel::prelude::*;
use kernel::sync::{Arc, Mutex};

use crate::cap::Capabilities;
use crate::rpbp::{self, MsgType, CHAN_CONTROL};
use crate::transport::{CompletionTable, Sequencer, RoundTrip};

/// Empty shim for the in-kernel CRC-32C. When the `kernel::crc32c`
/// binding is present (recent Rust-for-Linux trees expose it via
/// `rust/kernel/crc32c.rs`), replace this body with a call into that
/// module. Until then, an inline software implementation keeps the
/// driver self-contained and byte-identical with the firmware.
///
/// Table and algorithm are slice-by-1 over the Castagnoli polynomial
/// 0x1EDC6F41 (reflected 0x82F63B78).
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
            c = if (c & 1) != 0 {
                (c >> 1) ^ 0x82F63B78
            } else {
                c >> 1
            };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
};

/* ───── Per-device state ───────────────────────────────────────────── */

/// Inner mutable state guarded by `RpbridgeState.inner`. Split out so
/// callers hold the mutex only over the bookkeeping, not over the USB
/// transfer itself — the transfer anchors a URB and sleeps on a
/// wait-queue; holding a mutex across the sleep would dead-lock the
/// RX path that wakes it.
pub(crate) struct Inner {
    pub(crate) ctrl_seq:     Sequencer,
    pub(crate) completions:  CompletionTable,
    pub(crate) caps:         Option<Capabilities>,
}

impl Inner {
    pub(crate) fn new() -> Self {
        Self {
            ctrl_seq:    Sequencer::new(),
            completions: CompletionTable::new(),
            caps:        None,
        }
    }
}

/// Per-device state held in an `Arc` so auxiliary_device children can
/// reference it without lifetime acrobatics. Children call
/// `round_trip()` / `submit_oneway()` on this struct to issue RPBP
/// requests; the `RoundTrip` impl blocks on a per-slot wait-queue and
/// returns the parsed CMD_RESPONSE.
pub(crate) struct RpbridgeState {
    /// Mutable book-keeping. The USB transfer itself happens under the
    /// `usb_core` anchor API, not under this lock.
    pub(crate) inner: Mutex<Inner>,

    // TODO §R4L-USB-BINDINGS:
    //   Expect these fields once kernel::usb exposes URB + endpoint
    //   abstractions (planned for 6.14–6.15 Rust-for-Linux bump):
    //
    //   pub(crate) bulk_in_ep:  kernel::usb::Endpoint,
    //   pub(crate) bulk_out_ep: kernel::usb::Endpoint,
    //   pub(crate) urb_anchor:  kernel::usb::Anchor,
    //   pub(crate) rx_done:     kernel::sync::CondVar,
    //   pub(crate) intf:        kernel::usb::Interface,
    //
    //   RX URBs are resubmitted inside on_rx_complete(); on disconnect
    //   we call usb_kill_anchored_urbs(&urb_anchor) to drain them.
}

impl RpbridgeState {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Ok(Arc::new(
            RpbridgeState {
                inner: new_mutex!(Inner::new(), "rpbridge.inner"),
            },
            GFP_KERNEL,
        )?)
    }
}

/* ───── RoundTrip impl — the piece subsystem children call ─────────── */

impl RoundTrip for Arc<RpbridgeState> {
    fn round_trip(&self, channel: u16, request: &[u8])
        -> Result<(rpbp::Status, KVec<u8>), i32>
    {
        // 1. Reserve a seq + slot under the lock. Hold the lock only
        //    for allocation, never across USB submission.
        let (seq, slot_idx) = {
            let mut g = self.inner.lock();
            if channel != CHAN_CONTROL {
                // Stream channels use their own sequencer instances.
                // Round-trip semantics on them are a programmer error.
                return Err(crate::errno::EINVAL);
            }
            let seq = g.ctrl_seq.take();
            let idx = g.completions.allocate(channel, seq)?;
            (seq, idx)
        };

        // 2. Build the outgoing frame. CMD_REQUEST on the control
        //    channel, binary payload.
        let mut buf = [0u8; rpbp::MAX_FRAME_LEN];
        let mut hdr = rpbp::Header {
            magic:        rpbp::MAGIC,
            version:      rpbp::VERSION,
            msg_type:     MsgType::CmdRequest as u8,
            flags:        rpbp::Flags::empty(),
            channel,
            seq,
            payload_len:  0, // set by build_frame
            timestamp_us: 0,
        };
        let n = match rpbp::build_frame(&mut buf, &mut hdr, request) {
            Ok(n) => n,
            Err(e) => {
                let mut g = self.inner.lock();
                g.completions.cancel(slot_idx);
                return Err(e);
            }
        };

        // 3. Submit the bulk-OUT URB.
        //
        // TODO §R4L-USB-BINDINGS: replace with
        //
        //   kernel::usb::bulk_msg(
        //       &self.intf.usb_dev(),
        //       self.bulk_out_ep.addr(),
        //       &buf[..n],
        //       USB_CTRL_TIMEOUT_MSEC,
        //   )?
        //
        // until the binding lands, synchronous submission is impossible
        // from pure Rust code. `submit_frame_blocking` is a named hook
        // the kernel wrapper overrides.
        self.submit_frame_blocking(&buf[..n])?;

        // 4. Block until the RX pump fulfills or timeout fires.
        //
        // TODO §R4L-USB-BINDINGS: replace with
        //
        //   self.rx_done.wait_timeout(&mut self.inner,
        //                             Duration::from_millis(200))
        //
        // On wake-up, take_result() either returns the payload or
        // EAGAIN (spurious wake-up → loop back to wait).
        let (status, payload) = self.wait_for_completion(slot_idx, 200)?;
        Ok((status, payload))
    }

    fn submit_oneway(&self, _channel: u16, frame: &[u8]) -> Result<(), i32> {
        self.submit_frame_blocking(frame)
    }
}

impl RpbridgeState {
    /// Synchronous bulk-OUT submission. Blocking (caller must not hold
    /// the inner lock). Kernel wrapper implements this once URB
    /// bindings are available; the current body returns `ENOSYS` so
    /// the code path is exercised on host unit tests without silently
    /// succeeding against a non-functional transport.
    fn submit_frame_blocking(&self, _frame: &[u8]) -> Result<(), i32> {
        // TODO §R4L-USB-BINDINGS — see RoundTrip::round_trip() comment.
        Err(crate::errno::ENOSYS.into())
    }

    /// Block until `idx` is fulfilled or `timeout_ms` passes. Returns
    /// `ETIMEDOUT` on timeout (and marks the slot dead so a late
    /// response gets dropped). Spurious wake-ups loop.
    fn wait_for_completion(
        &self, idx: crate::transport::SlotIdx, timeout_ms: u32,
    ) -> Result<(rpbp::Status, KVec<u8>), i32> {
        // TODO §R4L-USB-BINDINGS: implement with kernel::sync::CondVar
        // wait_timeout. The stub returns ETIMEDOUT immediately so the
        // caller surface is exercised — behind-the-scenes plumbing is
        // the piece blocked on bindings.
        let _ = (idx, timeout_ms);
        Err(crate::errno::ETIMEDOUT.into())
    }

    /// Called by the RX URB-complete handler for every validated
    /// frame that came off the wire. Fulfils the matching slot and
    /// wakes its waiter. Channel 1 events and stream-data frames are
    /// routed here too — they go to per-subsystem pumps without
    /// touching the completion table.
    pub(crate) fn dispatch_rx_frame(&self, frame: &[u8]) -> Result<(), i32> {
        let hdr = rpbp::validate_frame(frame)?;
        let payload = rpbp::payload_slice(frame, &hdr);

        match rpbp::MsgType::try_from(hdr.msg_type)? {
            MsgType::CmdResponse => self.handle_cmd_response(&hdr, payload),
            MsgType::Error       => self.handle_error(&hdr, payload),
            MsgType::Event       => self.handle_event(&hdr, payload),
            MsgType::StreamData  => self.handle_stream_data(&hdr, payload),
            MsgType::Pong        |
            MsgType::Hello       |
            MsgType::Capabilities |
            MsgType::TimeSync    => {
                // Setup-time frames; the probe() state machine handles
                // these synchronously, RX pump ignores.
                Ok(())
            }
            _ => Err(crate::errno::EPROTO),
        }
    }

    fn handle_cmd_response(&self, hdr: &rpbp::Header, payload: &[u8]) -> Result<(), i32> {
        let resp = rpbp::parse_cmd_response_bin(payload)?;
        let mut out = KVec::new();
        out.extend_from_slice(resp.result, GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        let mut g = self.inner.lock();
        let _idx = g.completions.fulfill(hdr.channel, hdr.seq, resp.status, out)?;
        // TODO §R4L-USB-BINDINGS: self.rx_done.notify_all();
        Ok(())
    }

    fn handle_error(&self, _hdr: &rpbp::Header, payload: &[u8]) -> Result<(), i32> {
        // ERROR payload: status(u8) orig_channel(LE u16) orig_seq(LE u16) reason_len(LE u16) reason[...]
        if payload.len() < 7 { return Err(crate::errno::EPROTO); }
        let status = rpbp::Status::try_from(payload[0])?;
        let orig_channel = rpbp::rd_le16(&payload[1..3]);
        let orig_seq     = rpbp::rd_le16(&payload[3..5]);
        let mut g = self.inner.lock();
        let _ = g.completions.fulfill(orig_channel, orig_seq, status, KVec::new());
        // TODO §R4L-USB-BINDINGS: self.rx_done.notify_all();
        Ok(())
    }

    fn handle_event(&self, _hdr: &rpbp::Header, _payload: &[u8]) -> Result<(), i32> {
        // Events route to per-subsystem pumps (GPIO edge, bus fault…).
        // Until the aux children are wired, drop silently.
        Ok(())
    }

    fn handle_stream_data(&self, _hdr: &rpbp::Header, _payload: &[u8]) -> Result<(), i32> {
        // Stream data routes to the stream client that opened the
        // channel. IIO streaming is the first consumer.
        Ok(())
    }
}

/* ───── Capability-driven aux_device spawn ─────────────────────────── */

/// Walk the parsed capability map and spawn one aux_device per bus
/// whose access is "vendor" (RPBP) or "cdc+vendor". Called from
/// probe() once CAPABILITIES has completed and state.inner.caps is
/// populated. UART and CAN are skipped per ADR-0006 — they are bound
/// by `usbser` and `gs_usb.ko` respectively.
///
/// Returns the number of children spawned, or an error from the first
/// child that failed to register. Partial spawn on failure is
/// unwound by the caller via drop-of-state.
#[allow(dead_code)]
pub(crate) fn spawn_children(state: Arc<RpbridgeState>) -> Result<usize, i32> {
    // Take the caps out under the lock then drop it — Child::probe()
    // issues round-trips, and the inner lock must not be held across
    // a USB transfer.
    let caps_snapshot = {
        let g = state.inner.lock();
        match g.caps.as_ref() {
            Some(c) => clone_caps_for_spawn(c),
            None => return Err(crate::errno::EPROTO),
        }
    };

    let mut spawned = 0usize;
    for (class, idx) in caps_snapshot.iter() {
        // Skip CDC-routed classes — ADR-0006 makes this a hard rule.
        let is_vendor = match class.as_slice() {
            b"uart" | b"can" => false,
            b"gpio" | b"i2c" | b"spi" | b"pwm" | b"adc" => true,
            _ => false,
        };
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

        // TODO §R4L-AUX-BINDINGS: each of these will return an
        //   `auxiliary_device` handle that gets wrapped in a
        //   `struct aux_device_wrap` and registered via
        //   auxiliary_device_register(). Until those bindings land,
        //   we construct the child in-place to exercise the probe
        //   path end-to-end.
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

/// Extract (class, idx) pairs from the Capabilities so the spawn
/// loop doesn't hold an Arc<Capabilities> across round-trips. The
/// child's own probe() re-reads the full BusCaps from state if it
/// needs pin lists or frequency limits.
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

#[allow(dead_code)]
const RPBRIDGE_USB_VID: u16 = 0x1209; // pid.codes (change to allocated PID)
#[allow(dead_code)]
const RPBRIDGE_USB_PID: u16 = 0xFFFE; // pid.codes testing range

pub(crate) struct RpbridgeDriver;

#[vtable]
impl kernel::usb::Driver for RpbridgeDriver {
    type Data = Arc<RpbridgeState>;
    type IdInfo = ();

    const ID_TABLE: kernel::usb::IdTable<Self::IdInfo> =
        &kernel::usb::IdTable::new(&[
            // Match any interface of the composite device — we filter
            // on class/subclass inside probe().
            kernel::usb::DeviceId::new(RPBRIDGE_USB_VID, RPBRIDGE_USB_PID),
        ]);

    fn probe(
        _intf: &kernel::usb::Interface,
        _id_info: Option<&Self::IdInfo>,
    ) -> Result<Arc<RpbridgeState>> {
        pr_info!("rpbridge: probing USB interface\n");

        let state = RpbridgeState::new()?;

        // TODO §R4L-USB-BINDINGS: the probe() state machine sits here.
        //
        //   1. Walk _intf.endpoints(), pick the bulk-IN + bulk-OUT pair
        //      with EP_CLASS=0xFF / SUBCLASS=0x52 ('R') — RPBP vendor.
        //      If the interface is gs_usb (class 0xFF / subclass 0x43
        //      'C'), return -ENODEV so the mainline gs_usb driver can
        //      bind it instead.
        //
        //   2. Submit N (=4) bulk-IN URBs against state.urb_anchor.
        //      Each completion calls state.dispatch_rx_frame().
        //
        //   3. Send HELLO on control channel with a CBOR payload
        //      {"proto":[1,0,0], "host":{...}, "nonce":h'...'}.
        //      Wait for HELLO reply (msg_type=0x00, channel=0, seq=0).
        //
        //   4. Send CMD_REQUEST{SYS, GET_CAPABILITIES}. Parse the
        //      CBOR reply with crate::cap::Capabilities::from_cbor()
        //      and store it in state.inner.caps.
        //
        //   5. For each bus in caps.buses with access ∈ {"vendor",
        //      "cdc+vendor"}, call auxiliary_device_register on a
        //      child whose name matches the subsystem class:
        //
        //          "rpbridge-gpio.N"  where N = caps.buses[i].idx
        //          "rpbridge-i2c.N"
        //          "rpbridge-spi.N"
        //          "rpbridge-pwm.N"
        //          "rpbridge-iio.N"
        //
        //      Each child auxiliary_driver is match'd by its own
        //      id_table in {gpio,i2c,spi,pwm,iio}.rs.
        //
        //   6. UART and CAN are skipped explicitly (ADR-0006).

        Ok(state)
    }

    fn disconnect(_intf: &kernel::usb::Interface, _data: Arc<RpbridgeState>) {
        pr_info!("rpbridge: interface disconnected\n");
        // TODO §R4L-USB-BINDINGS:
        //   usb_kill_anchored_urbs(&state.urb_anchor);
        //   for child in state.aux_children { auxiliary_device_unregister(child) }
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

/* ───── Self-tests ─────────────────────────────────────────────────── */

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32c_empty_equals_zero() {
        assert_eq!(crc32c_slice(&[]), 0);
    }

    #[test]
    fn crc32c_known_vector() {
        // CRC-32C of ASCII "123456789" is 0xE3069283 per RFC 3720.
        assert_eq!(crc32c_slice(b"123456789"), 0xE306_9283);
    }

    #[test]
    fn header_roundtrip() {
        let h = rpbp::Header {
            magic: rpbp::MAGIC,
            version: rpbp::VERSION,
            msg_type: rpbp::MsgType::Ping as u8,
            flags: rpbp::Flags::CBOR,
            channel: 0,
            seq: 42,
            payload_len: 0,
            timestamp_us: 0x1234_5678,
        };
        let mut buf = [0u8; rpbp::HEADER_LEN];
        h.encode(&mut buf);
        let parsed = rpbp::Header::parse(&buf).unwrap();
        assert_eq!(parsed.seq, 42);
        assert_eq!(parsed.timestamp_us, 0x1234_5678);
    }
}
