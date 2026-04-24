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

/* ═════ Layer 3 — auxiliary_device adapter ════════════════════════ */

/// Per-child descriptor captured at `auxiliary_device_register()`
/// time. Held under Arc so it can survive across kernel callbacks.
pub(crate) struct GpioChild {
    #[allow(dead_code)]
    client: Client<RpbridgeState>,
    /// Pin whitelist discovered at probe time from a LIST round-trip.
    /// Indices into this vec are the gpiochip's logical line numbers;
    /// the firmware understands the raw RP2350 GPIO numbers.
    #[allow(dead_code)]
    pins:   Vec<u8>,
    /// Logical instance idx — matches BusCaps.idx for disambiguation
    /// when auxiliary_bus assigns multiple children of the same class.
    #[allow(dead_code)]
    idx:    u16,
}

impl GpioChild {
    /// Called from the core driver's `spawn_children` pass once per
    /// bus entry advertised in the capability map. The caller is
    /// expected to wrap the returned child in `auxiliary_device` and
    /// register it with `auxiliary_device_register()`.
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state);
        // Kick off a LIST immediately: gives us the authoritative pin
        // set the firmware exposes right now. Fall back to the
        // capability map's `pins` array if the round-trip fails
        // (e.g. the device is still in HELLO — probe ordering matters).
        let pins = client.list().unwrap_or_else(|_| {
            let mut v = Vec::new();
            for &p in bus.pins.iter() { let _ = v.push(p, GFP_KERNEL); }
            v
        });
        pr_info!("rpbridge-gpio.{}: {} pin(s) discovered\n",
                 bus.idx, pins.len());

        // TODO §R4L-GPIO-BINDINGS:
        //
        //   Once kernel::gpio::Chip is wired up in the target kernel
        //   (6.14+ has an early version in drivers/gpio/rust/),
        //   register a Chip whose vtable is:
        //
        //     get:           |pin| client.get(pins[pin as usize])
        //     set:           |pin, val| client.set(pins[pin as usize], val)
        //     direction_in:  |pin| client.configure(pins[i], DIR_INPUT, PULL_NONE)
        //     direction_out: |pin, val| { configure + set }
        //
        //   IRQ support needs kernel::gpio::IrqChip (nested irq_chip).
        //   Its request_resources / release_resources callbacks call
        //   client.watch(pin, edges) / client.unwatch(pin). The event-
        //   channel pump in core_driver.rs dispatches parsed EdgeEvent
        //   payloads to handle_nested_irq(virq) for this chip.

        Ok(GpioChild { client, pins, idx: bus.idx })
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
