// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-gpio — RPBP GPIO subsystem client + auxiliary gpio_chip
// child. See docs/protocol/subsystems/gpio.md for the wire format.
//
// Three layers:
//
//   1. `wire::*` — RPBP request builders + response parsers.
//      Kernel-binding agnostic; pure Rust. Exhaustively testable.
//
//   2. `Client` — a thin adapter that pairs the wire layer with a
//      `transport::RoundTrip` handle. Every public method issues one
//      RPBP round-trip and returns the decoded result as a Rust type.
//      Also kernel-binding agnostic.
//
//   3. `GpioChild` — the Rust-for-Linux `gpio_chip` adapter. Registers
//      a `gpio_chip`, wires get/set/direction/pull to the Client, and
//      plugs an `irq_chip` on top for edge events delivered on the
//      RPBP events channel.

use kernel::prelude::*;
use kernel::sync::Arc;

use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;
use crate::rpbp::{CHAN_CONTROL, Status};
use crate::transport::RoundTrip;

/* ═════ Layer 1 — wire (kernel-binding agnostic) ══════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::gpio as go};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Gpio as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* ───── 0x00 LIST ────────────────────────────────────────────── */

    pub fn build_list(dst: &mut [u8]) -> Result<usize, i32> {
        start(dst, go::LIST)
    }

    pub fn parse_list(resp: &[u8], out: &mut Vec<u8>) -> Result<(), i32> {
        if resp.is_empty() { return Err(crate::errno::EPROTO); }
        let count = resp[0] as usize;
        if resp.len() < 1 + count { return Err(crate::errno::EPROTO); }
        out.clear();
        out.reserve(count, GFP_KERNEL)?;
        for &p in &resp[1..1 + count] {
            out.push(p, GFP_KERNEL)?;
        }
        Ok(())
    }

    /* ───── 0x01 CONFIGURE ───────────────────────────────────────── */

    pub fn build_configure(
        dst: &mut [u8], pin: u8, dir: u8, pull: u8,
    ) -> Result<usize, i32> {
        let n = start(dst, go::CONFIGURE)?;
        if dst.len() < n + 3 { return Err(crate::errno::EINVAL); }
        dst[n]     = pin;
        dst[n + 1] = dir;
        dst[n + 2] = pull;
        Ok(n + 3)
    }

    /* ───── 0x02 SET ─────────────────────────────────────────────── */

    pub fn build_set(dst: &mut [u8], pin: u8, level: bool) -> Result<usize, i32> {
        let n = start(dst, go::SET)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        dst[n]     = pin;
        dst[n + 1] = if level { 1 } else { 0 };
        Ok(n + 2)
    }

    /* ───── 0x03 GET ─────────────────────────────────────────────── */

    pub fn build_get(dst: &mut [u8], pin: u8) -> Result<usize, i32> {
        let n = start(dst, go::GET)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        Ok(n + 1)
    }

    pub fn parse_get(resp: &[u8]) -> Result<bool, i32> {
        if resp.is_empty() { return Err(crate::errno::EPROTO); }
        Ok(resp[0] != 0)
    }

    /* ───── 0x04 SET_MASK ────────────────────────────────────────── */

    pub fn build_set_mask(dst: &mut [u8], mask: u64, values: u64) -> Result<usize, i32> {
        let n = start(dst, go::SET_MASK)?;
        if dst.len() < n + 16 { return Err(crate::errno::EINVAL); }
        rpbp::wr_le64(&mut dst[n..], mask);
        rpbp::wr_le64(&mut dst[n + 8..], values);
        Ok(n + 16)
    }

    /* ───── 0x05 GET_MASK ────────────────────────────────────────── */

    pub fn build_get_mask(dst: &mut [u8], mask: u64) -> Result<usize, i32> {
        let n = start(dst, go::GET_MASK)?;
        if dst.len() < n + 8 { return Err(crate::errno::EINVAL); }
        rpbp::wr_le64(&mut dst[n..], mask);
        Ok(n + 8)
    }

    pub fn parse_get_mask(resp: &[u8]) -> Result<u64, i32> {
        if resp.len() < 8 { return Err(crate::errno::EPROTO); }
        Ok(rpbp::rd_le64(resp))
    }

    /* ───── 0x06 WATCH ───────────────────────────────────────────── */

    pub fn build_watch(dst: &mut [u8], pin: u8, edges: u8) -> Result<usize, i32> {
        let n = start(dst, go::WATCH)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        dst[n]     = pin;
        dst[n + 1] = edges;
        Ok(n + 2)
    }

    /* ───── 0x07 UNWATCH ─────────────────────────────────────────── */

    pub fn build_unwatch(dst: &mut [u8], pin: u8) -> Result<usize, i32> {
        let n = start(dst, go::UNWATCH)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        Ok(n + 1)
    }

    /* ───── Edge event on channel 1 (RPBP EVENT) ─────────────────── */

    pub struct EdgeEvent {
        pub pin:   u8,
        pub level: u8,
        pub ts_us: u32,
    }

    pub fn parse_edge_event(payload: &[u8]) -> Result<EdgeEvent, i32> {
        if payload.len() < 8 { return Err(crate::errno::EPROTO); }
        if payload[0] != rpbp::Subsys::Gpio as u8 { return Err(crate::errno::EPROTO); }
        if payload[1] != 0x00                     { return Err(crate::errno::EPROTO); }
        Ok(EdgeEvent {
            pin:   payload[2],
            level: payload[3],
            ts_us: rpbp::rd_le32(&payload[4..8]),
        })
    }
}

/* ═════ Layer 2 — RPBP client (kernel-binding agnostic) ═══════════ */

/// Typed wrapper that binds the wire layer to a `RoundTrip` handle.
/// Every method performs one RPBP round-trip and returns the result
/// as a Rust type. Errors are Linux errnos so the gpio_chip callbacks
/// can return them verbatim.
pub struct Client<T: RoundTrip + ?Sized> {
    rt: Arc<T>,
}

impl<T: RoundTrip + ?Sized> Client<T> {
    pub fn new(rt: Arc<T>) -> Self { Self { rt } }

    fn rt_call(&self, req: &[u8]) -> Result<KVec<u8>, i32> {
        let (status, payload) = self.rt.round_trip(CHAN_CONTROL, req)?;
        if !matches!(status, Status::Ok) {
            return Err(status.to_errno());
        }
        Ok(payload)
    }

    pub fn list(&self) -> Result<Vec<u8>, i32> {
        let mut buf = [0u8; 2];
        let n = wire::build_list(&mut buf)?;
        let resp = self.rt_call(&buf[..n])?;
        let mut out = Vec::new();
        wire::parse_list(resp.as_slice(), &mut out)?;
        Ok(out)
    }

    pub fn configure(&self, pin: u8, dir: u8, pull: u8) -> Result<(), i32> {
        let mut buf = [0u8; 5];
        let n = wire::build_configure(&mut buf, pin, dir, pull)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn set(&self, pin: u8, level: bool) -> Result<(), i32> {
        let mut buf = [0u8; 4];
        let n = wire::build_set(&mut buf, pin, level)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn get(&self, pin: u8) -> Result<bool, i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_get(&mut buf, pin)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_get(resp.as_slice())
    }

    pub fn set_mask(&self, mask: u64, values: u64) -> Result<(), i32> {
        let mut buf = [0u8; 18];
        let n = wire::build_set_mask(&mut buf, mask, values)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn get_mask(&self, mask: u64) -> Result<u64, i32> {
        let mut buf = [0u8; 10];
        let n = wire::build_get_mask(&mut buf, mask)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_get_mask(resp.as_slice())
    }

    pub fn watch(&self, pin: u8, edges: u8) -> Result<(), i32> {
        let mut buf = [0u8; 4];
        let n = wire::build_watch(&mut buf, pin, edges)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn unwatch(&self, pin: u8) -> Result<(), i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_unwatch(&mut buf, pin)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }
}

/* ═════ Layer 3 — gpio_chip kernel integration ═══════════════════ */

use crate::ffi;
use crate::rpbp::op::gpio as go;

/// Backend the C trampolines call back into. Holds the Client + the
/// authoritative pin whitelist discovered at probe time.
struct GpioBackend {
    client: Client<RpbridgeState>,
    pins:   Vec<u8>,
}

impl ffi::gpio::GpioBackend for GpioBackend {
    fn get(&self, line: u32) -> i32 {
        let Some(&pin) = self.pins.as_slice().get(line as usize) else {
            return -crate::errno::ENODEV;
        };
        match self.client.get(pin) {
            Ok(v)   => if v { 1 } else { 0 },
            Err(_)  => -crate::errno::EIO,
        }
    }
    fn set(&self, line: u32, value: bool) {
        let Some(&pin) = self.pins.as_slice().get(line as usize) else { return; };
        let _ = self.client.set(pin, value);
    }
    fn direction_input(&self, line: u32) -> i32 {
        let Some(&pin) = self.pins.as_slice().get(line as usize) else {
            return -crate::errno::ENODEV;
        };
        match self.client.configure(pin, go::DIR_INPUT, go::PULL_NONE) {
            Ok(())  => 0,
            Err(e)  => -e,
        }
    }
    fn direction_output(&self, line: u32, value: bool) -> i32 {
        let Some(&pin) = self.pins.as_slice().get(line as usize) else {
            return -crate::errno::ENODEV;
        };
        if let Err(e) = self.client.configure(pin, go::DIR_OUTPUT, go::PULL_NONE) {
            return -e;
        }
        if let Err(e) = self.client.set(pin, value) {
            return -e;
        }
        0
    }
    fn request(&self, _line: u32) -> i32 {
        /* Pin-broker lives on the firmware side; any conflict comes
         * back through configure() as EBUSY. Nothing to do here. */
        0
    }
    fn release(&self, _line: u32) { /* as above */ }
}

/// Registered GPIO subsystem child. Drop unregisters the chip and
/// reclaims the backend.
pub(crate) struct GpioChild {
    _handle: ffi::gpio::ChipHandle,
    #[allow(dead_code)]
    idx:     u16,
}

impl GpioChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state.clone());
        // Kick off a LIST: gives us the authoritative pin set. Fall
        // back to the capability map's `pins` array if the round-trip
        // fails (e.g. the device is mid-handshake).
        let pins = client.list().unwrap_or_else(|_| {
            let mut v = Vec::new();
            for &p in bus.pins.iter() { let _ = v.push(p, GFP_KERNEL); }
            v
        });
        let ngpio = pins.len() as u16;
        pr_info!("rpbridge-gpio.{}: {} line(s)\n", bus.idx, ngpio);

        // Label is a static null-terminated string the kernel keeps
        // a borrow of; a format-derived string would need different
        // storage. For M11 we register both instances under the same
        // label — the .idx field disambiguates internally.
        let label = c"rpbridge-gpio";

        let parent = crate::ffi::auxiliary::parent_device(
            &*core::ops::Deref::deref(&state)
        ).map_err(|e| kernel::error::Error::from_errno(e))?;

        let backend = GpioBackend { client, pins };
        let handle = ffi::gpio::ChipHandle::register(parent, label, ngpio, backend)
            .map_err(|e| kernel::error::Error::from_errno(e))?;

        Ok(GpioChild { _handle: handle, idx: bus.idx })
    }
}

/* ═════ Tests ═════════════════════════════════════════════════════ */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpbp::op::gpio as go;

    #[test]
    fn build_list_is_minimal() {
        let mut buf = [0u8; 2];
        let n = wire::build_list(&mut buf).unwrap();
        assert_eq!(n, 2);
        assert_eq!(buf, [0x03, go::LIST]);
    }

    #[test]
    fn build_configure_encodes_all_args() {
        let mut buf = [0u8; 5];
        let n = wire::build_configure(&mut buf, 30, go::DIR_OUTPUT, go::PULL_UP).unwrap();
        assert_eq!(n, 5);
        assert_eq!(buf, [0x03, go::CONFIGURE, 30, go::DIR_OUTPUT, go::PULL_UP]);
    }

    #[test]
    fn parse_get_distinguishes_levels() {
        assert_eq!(wire::parse_get(&[0]).unwrap(), false);
        assert_eq!(wire::parse_get(&[1]).unwrap(), true);
        assert_eq!(wire::parse_get(&[42]).unwrap(), true);
        assert!(wire::parse_get(&[]).is_err());
    }

    #[test]
    fn mask_roundtrip_le64() {
        let mut buf = [0u8; 18];
        let n = wire::build_set_mask(&mut buf, 0xFF, 0x0A).unwrap();
        assert_eq!(n, 18);
        // Mask bytes at offset 2..10, values at 10..18, both LE.
        assert_eq!(&buf[2..10], &[0xFF, 0, 0, 0, 0, 0, 0, 0]);
        assert_eq!(&buf[10..18], &[0x0A, 0, 0, 0, 0, 0, 0, 0]);
    }

    #[test]
    fn edge_event_parses_with_timestamp() {
        // [subsys=3][event=0][pin=30][level=1][ts_us LE u32 = 0xDEADBEEF]
        let payload = [0x03, 0x00, 30, 1, 0xEF, 0xBE, 0xAD, 0xDE];
        let ev = wire::parse_edge_event(&payload).unwrap();
        assert_eq!(ev.pin, 30);
        assert_eq!(ev.level, 1);
        assert_eq!(ev.ts_us, 0xDEAD_BEEF);
    }

    #[test]
    fn edge_event_rejects_wrong_subsys() {
        let payload = [0x01, 0x00, 30, 1, 0, 0, 0, 0];
        assert!(wire::parse_edge_event(&payload).is_err());
    }
}

/* ═════ Client-level integration tests (MockRoundTrip) ═══════════ */

#[cfg(test)]
mod client_tests {
    use super::*;
    use crate::transport::MockRoundTrip;
    use crate::rpbp::Status;

    fn setup() -> (Client<MockRoundTrip>, Arc<MockRoundTrip>) {
        let mock = Arc::new(MockRoundTrip::new());
        let client = Client::new(mock.clone());
        (client, mock)
    }

    #[test]
    fn list_sends_correct_request_and_parses_response() {
        let (client, mock) = setup();
        // 3 pins: 30, 31, 37
        mock.queue_response(Status::Ok, &[0x03, 30, 31, 37]);
        let pins = client.list().unwrap();
        assert_eq!(pins.as_slice(), &[30, 31, 37]);

        let req = mock.pop_request();
        assert_eq!(req.as_slice(), &[0x03, 0x00]); // subsys=3, opcode=LIST
    }

    #[test]
    fn list_empty_response_means_no_pins() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[0x00]); // count=0
        let pins = client.list().unwrap();
        assert!(pins.as_slice().is_empty());
    }

    #[test]
    fn configure_encodes_pin_dir_pull() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        client.configure(30, 1, 1).unwrap(); // OUTPUT, PULL_UP

        let req = mock.pop_request();
        assert_eq!(req.as_slice(), &[0x03, 0x01, 30, 1, 1]);
    }

    #[test]
    fn set_high_and_low() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        mock.queue_response(Status::Ok, &[]);
        client.set(30, true).unwrap();
        client.set(31, false).unwrap();

        let r1 = mock.pop_request();
        let r2 = mock.pop_request();
        assert_eq!(r1.as_slice(), &[0x03, 0x02, 30, 1]);
        assert_eq!(r2.as_slice(), &[0x03, 0x02, 31, 0]);
    }

    #[test]
    fn get_parses_high_low() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[1]);
        mock.queue_response(Status::Ok, &[0]);
        assert!(client.get(30).unwrap());
        assert!(!client.get(31).unwrap());
    }

    #[test]
    fn non_ok_status_translates_to_errno() {
        let (client, mock) = setup();
        mock.queue_response(Status::EBusy, &[]);
        let err = client.set(30, true).unwrap_err();
        assert_eq!(err, 16); // EBUSY
    }

    #[test]
    fn set_mask_roundtrip() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        client.set_mask(0x00FF, 0x0055).unwrap();
        let req = mock.pop_request();
        // subsys=3, opcode=SET_MASK, mask LE64, values LE64
        assert_eq!(req.as_slice()[0..2], [0x03, 0x04]);
        assert_eq!(&req.as_slice()[2..10],
                   &[0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]);
        assert_eq!(&req.as_slice()[10..18],
                   &[0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]);
    }

    #[test]
    fn get_mask_parses_le64() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0]);
        let v = client.get_mask(0xFFFF).unwrap();
        assert_eq!(v, 0xEFBEADDE);
    }

    #[test]
    fn watch_and_unwatch_roundtrips() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        mock.queue_response(Status::Ok, &[]);
        client.watch(30, 0x03).unwrap(); // BOTH edges
        client.unwatch(30).unwrap();

        let r1 = mock.pop_request();
        let r2 = mock.pop_request();
        assert_eq!(r1.as_slice(), &[0x03, 0x06, 30, 0x03]);
        assert_eq!(r2.as_slice(), &[0x03, 0x07, 30]);
    }

    #[test]
    fn configure_forwards_enotsup_on_protected_pin() {
        let (client, mock) = setup();
        mock.queue_response(Status::ENotSup, &[]);
        let err = client.configure(0, 0, 0).unwrap_err();
        assert_eq!(err, 95); // ENOTSUP
    }
}
