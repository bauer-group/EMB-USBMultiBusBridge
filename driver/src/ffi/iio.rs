// SPDX-License-Identifier: GPL-2.0
//
// Safe wrapper around Linux `iio_device` registration.
//
// This first cut covers single-shot reads (read_raw → RAW / SCALE /
// PROCESSED). Triggered-buffer streaming is a separate service that
// consumes STREAM_DATA frames off the USB IN endpoint via a kthread;
// it lives in a future M12 milestone.

use core::ffi::c_void;
use core::ptr::NonNull;

use kernel::prelude::*;

use crate::ffi::bindings as cc;

#[derive(Copy, Clone, Debug)]
pub enum IioInfo {
    Raw,
    Scale,
    Processed,
}

/// Result of a read_raw call. IIO's ABI uses (int, int2) to express
/// decimal fractions — RAW uses INT alone, SCALE uses INT_PLUS_MICRO
/// (val + val2 / 1e6), PROCESSED uses INT alone in our units (cdegC
/// for temperature, mV for voltage).
pub struct IioReadVal {
    pub val:   i32,
    pub val2:  i32,
    pub ret:   i32, /* IIO_VAL_* */
}

pub trait IioBackend: Send + Sync {
    fn read(&self, channel: i32, info: IioInfo) -> Result<IioReadVal, i32>;
}

pub struct DeviceHandle {
    indio: NonNull<cc::iio_dev>,
    backend_raw: *mut dyn IioBackend,
}

unsafe impl Send for DeviceHandle {}
unsafe impl Sync for DeviceHandle {}

impl DeviceHandle {
    pub fn register<B: IioBackend + 'static>(
        parent:       NonNull<cc::device>,
        num_channels: u32,
        backend:      B,
    ) -> Result<Self, i32> {
        let boxed: Box<dyn IioBackend> = Box::new(backend, GFP_KERNEL)
            .map_err(|_| cc::ENOMEM)?;
        let backend_raw: *mut dyn IioBackend = Box::into_raw(boxed);

        // SAFETY: parent live; shim allocates via devm.
        let indio = unsafe {
            cc::rpbridge_iio_device_new(
                parent.as_ptr(),
                num_channels,
                backend_raw as *mut c_void,
            )
        };
        let indio = NonNull::new(indio).ok_or(cc::ENOMEM)?;
        let rc = unsafe { cc::rpbridge_iio_device_register(indio.as_ptr()) };
        if rc != 0 {
            unsafe { cc::rpbridge_iio_device_free(indio.as_ptr()); }
            let _reclaimed = unsafe { Box::from_raw(backend_raw) };
            return Err(-rc);
        }
        Ok(DeviceHandle { indio, backend_raw })
    }
}

impl Drop for DeviceHandle {
    fn drop(&mut self) {
        // SAFETY: self.indio was returned by rpbridge_iio_device_new.
        // devm_iio_device_alloc means the parent's teardown frees the
        // allocation; we only need to unregister.
        unsafe { cc::rpbridge_iio_device_free(self.indio.as_ptr()); }
        let _reclaimed = unsafe { Box::from_raw(self.backend_raw) };
    }
}

/* ───── C → Rust trampoline ─────────────────────────────────────── */

fn cookie_as_backend<'a>(cookie: *mut c_void) -> &'a dyn IioBackend {
    // SAFETY: cookie is backend_raw, valid for the indio's life.
    unsafe { &*(cookie as *const dyn IioBackend) }
}

#[no_mangle]
pub extern "C" fn rpbridge_iio_cb_read_raw(
    cookie: *mut c_void,
    ch:     i32,
    info:   i32,
    val:    *mut i32,
    val2:   *mut i32,
) -> i32 {
    if val.is_null() || val2.is_null() { return -cc::EINVAL; }
    let backend = cookie_as_backend(cookie);
    let info = match info {
        x if x == cc::IIO_CHAN_INFO_RAW       => IioInfo::Raw,
        x if x == cc::IIO_CHAN_INFO_SCALE     => IioInfo::Scale,
        x if x == cc::IIO_CHAN_INFO_PROCESSED => IioInfo::Processed,
        _ => return -cc::EINVAL,
    };
    match backend.read(ch, info) {
        Ok(v) => {
            // SAFETY: kernel-supplied out pointers, guaranteed live.
            unsafe {
                *val  = v.val;
                *val2 = v.val2;
            }
            v.ret
        }
        Err(e) => -e,
    }
}
