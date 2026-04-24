// SPDX-License-Identifier: GPL-2.0
//
// Safe wrapper around the Linux `i2c_adapter` registration API.

use core::ffi::c_void;
use core::ptr::NonNull;

use kernel::prelude::*;

use crate::ffi::bindings as cc;

/// Direction of a single i2c message as seen from the bus master.
#[derive(Copy, Clone, Debug)]
pub enum MsgDir<'a> {
    Write(&'a [u8]),
    Read(&'a mut [u8]),
}

/// Per-message view the Rust backend receives. `addr` is 7-bit
/// (already stripped of the r/w bit by the i2c core).
pub struct I2cReq<'a> {
    pub addr:    u16,
    pub flags:   u16,
    pub data:    MsgDir<'a>,
}

/// Backend trait the Child implements. Returns the number of
/// messages successfully transferred on success, -errno on failure.
pub trait I2cBackend: Send + Sync {
    fn master_xfer(&self, msgs: &mut [I2cReq<'_>]) -> Result<usize, i32>;
    fn functionality(&self) -> u32;
}

/// RAII handle for a registered i2c_adapter.
pub struct AdapterHandle {
    adap: NonNull<cc::i2c_adapter>,
    backend_raw: *mut dyn I2cBackend,
}

unsafe impl Send for AdapterHandle {}
unsafe impl Sync for AdapterHandle {}

impl AdapterHandle {
    pub fn register<B: I2cBackend + 'static>(
        parent: NonNull<cc::device>,
        name:   &'static core::ffi::CStr,
        backend: B,
    ) -> Result<Self, i32> {
        let boxed: Box<dyn I2cBackend> = Box::new(backend, GFP_KERNEL)
            .map_err(|_| cc::ENOMEM)?;
        let backend_raw: *mut dyn I2cBackend = Box::into_raw(boxed);

        // SAFETY: parent is live; name is a static null-terminated
        // string; the shim copies the name into the adapter.
        let adap = unsafe {
            cc::rpbridge_i2c_adapter_new(
                parent.as_ptr(),
                name.as_ptr(),
                backend_raw as *mut c_void,
            )
        };
        let adap = NonNull::new(adap).ok_or(cc::ENOMEM)?;

        // SAFETY: adap was just allocated by the shim.
        let rc = unsafe { cc::rpbridge_i2c_adapter_register(adap.as_ptr()) };
        if rc != 0 {
            unsafe { cc::rpbridge_i2c_adapter_free(adap.as_ptr()); }
            let _reclaimed = unsafe { Box::from_raw(backend_raw) };
            return Err(-rc);
        }
        Ok(AdapterHandle { adap, backend_raw })
    }
}

impl Drop for AdapterHandle {
    fn drop(&mut self) {
        // i2c_del_adapter inside the shim drains in-flight callers
        // synchronously. After this returns no trampoline can race
        // the backend free.
        // SAFETY: self.adap was returned by rpbridge_i2c_adapter_new.
        unsafe { cc::rpbridge_i2c_adapter_free(self.adap.as_ptr()); }
        // SAFETY: self.backend_raw came from Box::into_raw.
        let _reclaimed = unsafe { Box::from_raw(self.backend_raw) };
    }
}

/* ───── C → Rust trampolines ────────────────────────────────────── */

fn cookie_as_backend<'a>(cookie: *mut c_void) -> &'a dyn I2cBackend {
    // SAFETY: cookie is the backend_raw pointer stored inside the
    // adapter, valid for the life of the registered adapter.
    unsafe { &*(cookie as *const dyn I2cBackend) }
}

#[no_mangle]
pub extern "C" fn rpbridge_i2c_cb_master_xfer(
    cookie: *mut c_void,
    msgs_ptr: *mut cc::i2c_msg,
    num: i32,
) -> i32 {
    if num <= 0 || msgs_ptr.is_null() { return -cc::EINVAL; }
    let backend = cookie_as_backend(cookie);

    // Build the Rust-side view of each msg. For reads we need a
    // mutable borrow of the kernel-supplied buffer; for writes an
    // immutable borrow. Every msg has independent buf storage so
    // the lifetimes don't alias.
    //
    // Because Rust doesn't let us build a Vec<I2cReq> holding
    // differently-mutable references safely, we process messages
    // one at a time and funnel each through a 1-element slice. That
    // matches the original kernel semantics (sequential master_xfer)
    // and keeps the borrow checker happy.

    let mut transferred = 0i32;
    for i in 0..num {
        // SAFETY: msgs_ptr[i] is within the kernel-supplied array
        // bounded by `num`. The shim accessors read msg fields.
        let msg = unsafe { msgs_ptr.offset(i as isize) };
        let addr = unsafe { cc::rpbridge_i2c_msg_addr(msg) };
        let flags = unsafe { cc::rpbridge_i2c_msg_flags(msg) };
        let len = unsafe { cc::rpbridge_i2c_msg_len(msg) } as usize;
        let buf = unsafe { cc::rpbridge_i2c_msg_buf(msg) };
        if len > 0 && buf.is_null() { return -cc::EINVAL; }

        let is_read = (flags & cc::I2C_M_RD) != 0;
        let result = if is_read {
            // SAFETY: kernel ensures buf points at `len` writable bytes
            // for the duration of this call.
            let slice = unsafe { core::slice::from_raw_parts_mut(buf, len) };
            let mut req = I2cReq { addr, flags, data: MsgDir::Read(slice) };
            backend.master_xfer(core::slice::from_mut(&mut req))
        } else {
            // SAFETY: kernel ensures buf points at `len` readable bytes.
            let slice = unsafe { core::slice::from_raw_parts(buf, len) };
            let mut req = I2cReq { addr, flags, data: MsgDir::Write(slice) };
            backend.master_xfer(core::slice::from_mut(&mut req))
        };

        match result {
            Ok(n) => { transferred += n as i32; }
            Err(e) => {
                if transferred > 0 { return transferred; }
                return -e;
            }
        }
    }
    transferred
}

#[no_mangle]
pub extern "C" fn rpbridge_i2c_cb_functionality(cookie: *mut c_void) -> u32 {
    cookie_as_backend(cookie).functionality()
}
