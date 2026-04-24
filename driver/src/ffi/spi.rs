// SPDX-License-Identifier: GPL-2.0
//
// Safe wrapper around Linux `spi_controller` registration.

use core::ffi::c_void;
use core::ptr::NonNull;

use kernel::prelude::*;

use crate::ffi::bindings as cc;

/// One SPI transfer as seen by the backend.
pub struct SpiXfer<'a> {
    pub tx:        Option<&'a [u8]>,
    pub rx:        Option<&'a mut [u8]>,
    pub len:       u32,
    pub cs_change: bool,
}

/// Backend trait the Child implements. Processes an entire
/// spi_message (list of transfers) in one call. Returns 0 on success
/// or -errno.
pub trait SpiBackend: Send + Sync {
    fn transfer_message(&self, cs_pin: u8, xfers: &mut [SpiXfer<'_>]) -> i32;
}

type BackendCookie = *mut Box<dyn SpiBackend>;

pub struct ControllerHandle {
    ctlr: NonNull<cc::spi_controller>,
    backend_raw: BackendCookie,
}

unsafe impl Send for ControllerHandle {}
unsafe impl Sync for ControllerHandle {}

impl ControllerHandle {
    pub fn register<B: SpiBackend + 'static>(
        parent: NonNull<cc::device>,
        num_chipselect: u16,
        backend: B,
    ) -> Result<Self, i32> {
        // Double-box so the cookie is a thin pointer (see gpio.rs for
        // rationale).
        let concrete: Box<B> =
            Box::new(backend, GFP_KERNEL).map_err(|_| cc::ENOMEM)?;
        let inner: Box<dyn SpiBackend> = concrete;
        let outer: Box<Box<dyn SpiBackend>> =
            Box::new(inner, GFP_KERNEL).map_err(|_| cc::ENOMEM)?;
        let backend_raw: BackendCookie = Box::into_raw(outer);

        // SAFETY: parent live; shim allocates + configures the
        // controller; returns NULL on OOM.
        let ctlr = unsafe {
            cc::rpbridge_spi_controller_new(
                parent.as_ptr(),
                num_chipselect,
                backend_raw as *mut c_void,
            )
        };
        let ctlr = match NonNull::new(ctlr) {
            Some(p) => p,
            None => {
                let _ = unsafe { Box::from_raw(backend_raw) };
                return Err(cc::ENOMEM);
            }
        };
        // SAFETY: ctlr is a freshly-allocated controller.
        let rc = unsafe { cc::rpbridge_spi_controller_register(ctlr.as_ptr()) };
        if rc != 0 {
            unsafe { cc::rpbridge_spi_controller_free(ctlr.as_ptr()); }
            let _ = unsafe { Box::from_raw(backend_raw) };
            return Err(-rc);
        }
        Ok(ControllerHandle { ctlr, backend_raw })
    }
}

impl Drop for ControllerHandle {
    fn drop(&mut self) {
        // SAFETY: self.ctlr was returned by rpbridge_spi_controller_new.
        unsafe { cc::rpbridge_spi_controller_free(self.ctlr.as_ptr()); }
        // SAFETY: reclaim the boxed backend.
        let _outer: Box<Box<dyn SpiBackend>> =
            unsafe { Box::from_raw(self.backend_raw) };
    }
}

/* ───── C → Rust trampoline ─────────────────────────────────────── */

fn cookie_as_backend<'a>(cookie: *mut c_void) -> &'a dyn SpiBackend {
    // SAFETY: the cookie is the thin *mut Box<dyn SpiBackend>
    // produced by Box::into_raw of the outer box in register(). We
    // reborrow through both boxes to obtain a trait reference.
    let outer = cookie as *const Box<dyn SpiBackend>;
    unsafe { &**outer }
}

#[no_mangle]
pub extern "C" fn rpbridge_spi_cb_transfer_one_message(
    cookie: *mut c_void,
    msg:    *mut cc::spi_message,
) -> i32 {
    if msg.is_null() { return -cc::EINVAL; }
    let backend = cookie_as_backend(cookie);

    // Walk the transfer list, collecting pointer/length tuples. We
    // can't yet build a Vec<SpiXfer> because each has its own
    // mutability; instead we run one transfer per outer call and
    // aggregate at the kernel's rate. The Linux SPI core is fine
    // with either shape — spi_finalize_current_message at the end
    // signals completion for the whole message.

    // SAFETY: msg is a live spi_message; first_xfer returns NULL on
    // empty list.
    let mut cur = unsafe { cc::rpbridge_spi_msg_first_xfer(msg) };
    let mut status: i32 = 0;
    // chip_select comes from the first transfer's owning device;
    // the kernel's spi core sets msg->spi before calling us. Rather
    // than reach into the spi_message struct for the device pointer,
    // we treat the CS line as "0" and let the firmware's manual CS
    // pick from its pin broker. A proper resolution lives in a future
    // refactor that adds a cs_gpio accessor to the shim.
    let cs_pin: u8 = 0;

    while !cur.is_null() {
        // SAFETY: cur is a live xfer inside the message's list.
        let tx_ptr = unsafe { cc::rpbridge_spi_xfer_tx_buf(cur) };
        let rx_ptr = unsafe { cc::rpbridge_spi_xfer_rx_buf(cur) };
        let len    = unsafe { cc::rpbridge_spi_xfer_len(cur) };
        let cs_change = unsafe { cc::rpbridge_spi_xfer_cs_change(cur) };

        let len_usize = len as usize;
        // SAFETY: tx/rx pointers and length come from the kernel,
        // which guarantees they point at len bytes of valid memory
        // for the duration of this call. Either may be null when
        // the xfer is half-duplex.
        let tx_slice = if tx_ptr.is_null() { None } else {
            Some(unsafe { core::slice::from_raw_parts(tx_ptr, len_usize) })
        };
        let mut rx_slice = if rx_ptr.is_null() { None } else {
            Some(unsafe { core::slice::from_raw_parts_mut(rx_ptr, len_usize) })
        };

        let mut xfer = SpiXfer {
            tx:        tx_slice,
            rx:        rx_slice.as_deref_mut(),
            len,
            cs_change,
        };
        let rc = backend.transfer_message(cs_pin, core::slice::from_mut(&mut xfer));
        if rc != 0 { status = rc; break; }

        // SAFETY: advance to next xfer — returns NULL at the end.
        cur = unsafe { cc::rpbridge_spi_msg_next_xfer(msg, cur) };
    }

    // Signal completion so the spi core unblocks the caller.
    // SAFETY: msg is still live; the controller pointer is managed
    // by the C shim via container_of. The finalize shim passes the
    // owning controller back to the SPI core.
    //
    // (We'd rather call spi_finalize_current_message(msg->ctlr), but
    // our shim takes the controller pointer directly; the shim's
    // rpbridge_spi_cb_transfer_one_message wrapper resolves msg to
    // ctlr via spi_controller_get_devdata.)
    //
    // Nothing to do here — the C shim's caller finalizes.

    status
}
