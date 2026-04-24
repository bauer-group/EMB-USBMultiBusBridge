// SPDX-License-Identifier: GPL-2.0
//
// Safe wrapper around the Linux USB core's synchronous bulk transfer
// API. Rust-for-Linux exposes `kernel::usb::Driver` in 6.18 but not
// `usb_bulk_msg` — until that lands, we call through to the C
// function via `ffi::bindings`.
//
// Two public types:
//
//   * `Interface` — typed wrapper around `*mut usb_interface` with
//     helpers to discover the RPBP vendor bulk-IN + bulk-OUT pair.
//
//   * `BulkEndpoint` — holds a (dev, pipe) tuple plus the max packet
//     size discovered during enumeration. Its `transfer` method is a
//     blocking `usb_bulk_msg` call with caller-supplied timeout.

use core::ffi::{c_int, c_uint, c_void};
use kernel::prelude::*;

use crate::ffi::bindings as cc;

/// USB pipe direction — just a type-safe view of the top bit of
/// bEndpointAddress.
#[derive(Copy, Clone, Debug)]
pub enum Direction { In, Out }

/// A discovered bulk endpoint. Pairs the usb_device pointer with the
/// pre-computed pipe index so each transfer is one unsafe call.
#[derive(Copy, Clone)]
pub struct BulkEndpoint {
    dev:           *mut cc::usb_device,
    pub address:   u8,
    pub pipe:      c_uint,
    pub direction: Direction,
    pub max_packet: u16,
}

// SAFETY: BulkEndpoint is `Send`-able across threads because it
// carries a raw pointer to the USB device whose lifetime is bounded
// by the parent `Interface` (held in RpbridgeState). The USB core's
// internal locking protects concurrent `usb_bulk_msg` calls.
unsafe impl Send for BulkEndpoint {}
unsafe impl Sync for BulkEndpoint {}

/// Typed wrapper around `*mut usb_interface`. Constructed from the
/// pointer the `kernel::usb::Driver::probe()` callback hands us.
pub struct Interface {
    ptr: *mut cc::usb_interface,
}

// SAFETY: as above — the USB core guarantees the interface pointer's
// validity across `probe` / `disconnect`.
unsafe impl Send for Interface {}
unsafe impl Sync for Interface {}

impl Interface {
    /// # Safety
    /// Caller must supply a valid `*mut usb_interface` whose lifetime
    /// is bounded by the `probe`/`disconnect` callback pair. Reference
    /// semantics: the kernel keeps the interface alive until
    /// `disconnect` returns.
    pub unsafe fn from_raw(ptr: *mut cc::usb_interface) -> Self {
        Self { ptr }
    }

    /// Walk the active altsetting's endpoint list. Returns every bulk
    /// endpoint found. Non-bulk endpoints (interrupt, isochronous,
    /// control) are skipped. The USB core has already validated the
    /// descriptor set by the time probe() is called, so this cannot
    /// fail structurally — the returned Vec may be empty if the
    /// interface has no bulk endpoints.
    pub fn bulk_endpoints(&self) -> Result<Vec<BulkEndpoint>, i32> {
        let mut out = Vec::new();
        // SAFETY: self.ptr is valid; the shim reads the altsetting's
        // descriptor fields which are stable across kernel versions.
        let n = unsafe { cc::rpbridge_intf_num_endpoints(self.ptr) };
        if n <= 0 { return Ok(out); }
        out.reserve(n as usize, GFP_KERNEL)
            .map_err(|_| cc::ENOMEM)?;

        // SAFETY: bounded iteration over a fixed n returned by the
        // kernel. Each accessor clamps its own index so we can't
        // read past the descriptor array.
        let dev = unsafe { cc::interface_to_usbdev(self.ptr) };

        for i in 0..n {
            let attrs = unsafe { cc::rpbridge_intf_endpoint_attributes(self.ptr, i) };
            let xfer_type = attrs & cc::USB_ENDPOINT_XFERTYPE_MASK;
            if xfer_type != cc::USB_ENDPOINT_XFER_BULK { continue; }

            let address = unsafe { cc::rpbridge_intf_endpoint_address(self.ptr, i) };
            let direction = if address & cc::USB_DIR_IN != 0 {
                Direction::In
            } else {
                Direction::Out
            };
            let ep_num = (address & 0x0F) as c_uint;
            let pipe = match direction {
                Direction::In  => unsafe { cc::rpbridge_usb_rcvbulkpipe(dev, ep_num) },
                Direction::Out => unsafe { cc::rpbridge_usb_sndbulkpipe(dev, ep_num) },
            };
            let max_packet = unsafe {
                cc::rpbridge_intf_endpoint_max_packet(self.ptr, i)
            };
            out.push(BulkEndpoint { dev, address, pipe, direction, max_packet },
                     GFP_KERNEL)
                .map_err(|_| cc::ENOMEM)?;
        }
        Ok(out)
    }

    /// Find the IN + OUT bulk endpoint pair matching the RPBP vendor
    /// interface. Rejects interfaces with 0 or > 2 bulk endpoints.
    pub fn rpbp_pair(&self) -> Result<(BulkEndpoint, BulkEndpoint), i32> {
        let eps = self.bulk_endpoints()?;
        let mut in_ep = None;
        let mut out_ep = None;
        for e in eps.iter() {
            match e.direction {
                Direction::In  if in_ep.is_none()  => in_ep  = Some(*e),
                Direction::Out if out_ep.is_none() => out_ep = Some(*e),
                _ => { /* unexpected extra endpoint */ }
            }
        }
        match (in_ep, out_ep) {
            (Some(i), Some(o)) => Ok((i, o)),
            _ => Err(cc::ENODEV),
        }
    }

    /// Raw pointer escape hatch for the shims that need it.
    pub fn as_ptr(&self) -> *mut cc::usb_interface { self.ptr }
}

impl BulkEndpoint {
    /// Blocking synchronous bulk transfer. Writes (OUT) or reads
    /// (IN) up to `buf.len()` bytes. Returns the actual byte count
    /// on success. `timeout_ms = 0` means no timeout.
    ///
    /// # Safety & semantics
    ///
    /// Matches `usb_bulk_msg`'s contract: caller owns `buf` for the
    /// duration of the call; the kernel copies into / out of it via
    /// its own URB submission.
    pub fn transfer(&self, buf: &mut [u8], timeout_ms: u32) -> Result<usize, i32> {
        if buf.is_empty() { return Ok(0); }
        let mut actual: c_int = 0;
        // SAFETY: self.dev is valid for the lifetime of the parent
        // Interface. buf is a &mut slice so exclusive access is
        // guaranteed by the borrow checker; the kernel receives a
        // raw pointer + length and respects both bounds per
        // usb_bulk_msg's contract.
        let rc = unsafe {
            cc::usb_bulk_msg(
                self.dev,
                self.pipe,
                buf.as_mut_ptr() as *mut c_void,
                buf.len() as c_int,
                &mut actual as *mut c_int,
                timeout_ms as c_int,
            )
        };
        if rc != 0 { return Err(-rc); }
        if actual < 0 { return Err(cc::EIO); }
        Ok(actual as usize)
    }
}
