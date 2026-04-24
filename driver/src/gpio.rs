// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-gpio — RPBP GPIO subsystem client + auxiliary gpio_chip
// child. See docs/protocol/subsystems/gpio.md for the wire format.
//
// This module is split into two layers:
//
//   1. `wire::*` — RPBP request builders + response parsers.
//      Kernel-binding agnostic; pure Rust. The kernel adapter calls
//      these helpers to turn "set pin 30 high" into RPBP bytes and
//      back. Exhaustively testable.
//
//   2. `GpioChild` / `probe()` — the Rust-for-Linux gpiochip adapter.
//      Registers a `gpio_chip`, wires get/set/direction/pull to the
//      wire layer, and plugs an `irq_chip` on top for edge events.
//
// Implementation bullet list (see firmware/TODO.md §M3 / §M6 for the
// matching firmware side):
//   - `kernel::gpio::Chip` with get/set, direction control, pull cfg
//   - `kernel::gpio::IrqChip` as nested irq_chip for edge/level IRQs
//   - edge events arrive on RPBP channel 1 (events) — see
//     `wire::parse_edge_event`. The adapter calls
//     `handle_nested_irq()` once per edge.
//   - debounce via a firmware-side parameter (not kernel software)
//   - pin-count and per-pin capability bitmap read from crate::cap

use kernel::prelude::*;

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

/* ═════ Layer 2 — auxiliary_device adapter (stub) ═════════════════ */

#[allow(dead_code)]
pub(crate) struct GpioChild;

#[allow(dead_code)]
impl GpioChild {
    pub(crate) fn probe() -> Result<Self> {
        pr_info!("rpbridge-gpio: probe (wire layer ready; chip adapter stub)\n");
        // TODO (blocked on kernel bindings upstreaming):
        //   - read chip.ngpio from crate::cap::Capabilities::buses_of_class("gpio")
        //   - register a kernel::gpio::Chip whose `get`/`set`/`direction_*`
        //     callbacks translate into wire::build_* + a synchronous
        //     RPBP round-trip against the parent RpbridgeState.
        //   - install a nested irq_chip; an events-channel RX dispatcher
        //     (per-device) calls wire::parse_edge_event and fires
        //     handle_nested_irq() for the matching virq.
        Ok(GpioChild)
    }
}
