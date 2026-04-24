// SPDX-License-Identifier: GPL-2.0
//
// Safe wrapper around the Linux `gpio_chip` registration API.
// Pairs with src/ffi/shims.c::rpbridge_gpio_*.
//
// ## Callback plumbing
//
// The C shim keeps the gpio_chip vtable (get, set, direction_input,
// …). Each vtable callback receives the chip pointer, extracts the
// `cookie` from `gpiochip_get_data(chip)`, and calls one of the
// Rust symbols this module exports via `#[no_mangle]`:
//
//   rpbridge_gpio_cb_get / set / direction_input / direction_output
//   / request / free
//
// The cookie is a `*mut GpioBackend` obtained from `Box::into_raw`.
// It is reclaimed exactly once — in `ChipHandle::drop` — after the
// chip has been unregistered, so no in-flight callback can race the
// free.

use core::ffi::c_void;
use core::ptr::NonNull;

use kernel::prelude::*;

use crate::ffi::bindings as cc;

/// Opaque cookie type handed to the C shim. The vtable trampolines
/// cast back to `&GpioBackend` inside their bodies.
pub trait GpioBackend: Send + Sync {
    fn get(&self, line: u32) -> i32;
    fn set(&self, line: u32, value: bool);
    fn direction_input(&self, line: u32) -> i32;
    fn direction_output(&self, line: u32, value: bool) -> i32;
    fn request(&self, line: u32) -> i32;
    fn release(&self, line: u32);
}

/// RAII handle for a registered gpio_chip. Drops unregister the
/// chip + reclaim the boxed backend.
pub struct ChipHandle {
    chip: NonNull<cc::gpio_chip>,
    // Kept as a raw pointer so its address stays stable while the
    // kernel holds on to it via the cookie. We reclaim it in Drop.
    backend_raw: *mut dyn GpioBackend,
}

unsafe impl Send for ChipHandle {}
unsafe impl Sync for ChipHandle {}

impl ChipHandle {
    /// Allocate, configure, and register a gpio_chip under `parent`.
    /// `label` is the chip name visible in /sys/class/gpio/<label>.
    /// `ngpio` is the number of logical lines the chip exposes.
    /// `backend` owns the Rust-side logic; it must outlive the
    /// registration (enforced by this type's Drop impl).
    pub fn register<B: GpioBackend + 'static>(
        parent: NonNull<cc::device>,
        label: &'static core::ffi::CStr,
        ngpio: u16,
        backend: B,
    ) -> Result<Self, i32> {
        // Box the backend so it has a stable address for the cookie.
        let boxed: Box<dyn GpioBackend> = Box::new(backend, GFP_KERNEL)
            .map_err(|_| cc::ENOMEM)?;
        let backend_raw: *mut dyn GpioBackend = Box::into_raw(boxed);

        // SAFETY: parent is a live device pointer; label is a static
        // null-terminated string; backend_raw owns the backing memory
        // until Drop.
        let chip = unsafe {
            cc::rpbridge_gpio_chip_new(
                parent.as_ptr(),
                label.as_ptr(),
                -1,            /* base = -1 → dynamic */
                ngpio,
                backend_raw as *mut c_void,
            )
        };
        let chip = NonNull::new(chip).ok_or(cc::ENOMEM)?;

        // SAFETY: chip was just allocated; `rpbridge_gpio_chip_register`
        // calls gpiochip_add_data which either succeeds or returns -errno.
        let rc = unsafe { cc::rpbridge_gpio_chip_register(chip.as_ptr()) };
        if rc != 0 {
            // Unwind: free the chip + reclaim the Box.
            unsafe { cc::rpbridge_gpio_chip_free(chip.as_ptr()) };
            // SAFETY: we just created this from Box::into_raw; no
            // vtable call has happened yet because registration
            // failed, so reclaiming is sound.
            let _reclaimed = unsafe { Box::from_raw(backend_raw) };
            return Err(-rc);
        }

        // Attach the nested irq_chip for WATCH events.
        let rc = unsafe { cc::rpbridge_gpio_girq_add(chip.as_ptr()) };
        if rc != 0 {
            // Not fatal — continue without IRQ support. Log.
            pr_info!("rpbridge-gpio: girq_add failed ({}); WATCH disabled\n", rc);
        }

        Ok(ChipHandle { chip, backend_raw })
    }

    pub fn as_ptr(&self) -> *mut cc::gpio_chip { self.chip.as_ptr() }
}

impl Drop for ChipHandle {
    fn drop(&mut self) {
        // Unregister first — the C core drains any in-flight vtable
        // callers synchronously and guarantees none runs after this
        // point.
        // SAFETY: self.chip was returned by rpbridge_gpio_chip_new.
        unsafe {
            cc::rpbridge_gpio_chip_unregister(self.chip.as_ptr());
            cc::rpbridge_gpio_chip_free(self.chip.as_ptr());
        }
        // Now it is safe to reclaim the backend. No vtable call can
        // see the dangling pointer because the chip is gone.
        // SAFETY: self.backend_raw came from Box::into_raw in
        // register(); we hand ownership back to the Box and it is
        // dropped on function exit.
        let _reclaimed = unsafe { Box::from_raw(self.backend_raw) };
    }
}

/* ───── C → Rust trampolines ────────────────────────────────────────
 *
 * These are the `#[no_mangle] extern "C"` symbols the shim's vtable
 * callbacks invoke. They recover the backend from `cookie` and
 * dispatch to the trait method.
 *
 * The shim guarantees `cookie` is non-null and valid for the
 * lifetime of the registered gpio_chip. We treat it as `&dyn
 * GpioBackend` — immutable borrow is sufficient because every trait
 * method takes `&self`.
 */

fn cookie_as_backend<'a>(cookie: *mut c_void) -> &'a dyn GpioBackend {
    // SAFETY: the cookie stored by gpiochip_get_data is the
    // backend_raw pointer we set in ChipHandle::register. It points
    // at a live `Box<dyn GpioBackend>` for the duration of the
    // registered chip's life.
    unsafe { &*(cookie as *const dyn GpioBackend) }
}

#[no_mangle]
pub extern "C" fn rpbridge_gpio_cb_get(cookie: *mut c_void, line: u32) -> i32 {
    cookie_as_backend(cookie).get(line)
}

#[no_mangle]
pub extern "C" fn rpbridge_gpio_cb_set(cookie: *mut c_void, line: u32, value: i32) {
    cookie_as_backend(cookie).set(line, value != 0)
}

#[no_mangle]
pub extern "C" fn rpbridge_gpio_cb_direction_input(
    cookie: *mut c_void, line: u32,
) -> i32 {
    cookie_as_backend(cookie).direction_input(line)
}

#[no_mangle]
pub extern "C" fn rpbridge_gpio_cb_direction_output(
    cookie: *mut c_void, line: u32, value: i32,
) -> i32 {
    cookie_as_backend(cookie).direction_output(line, value != 0)
}

#[no_mangle]
pub extern "C" fn rpbridge_gpio_cb_request(
    cookie: *mut c_void, line: u32,
) -> i32 {
    cookie_as_backend(cookie).request(line)
}

#[no_mangle]
pub extern "C" fn rpbridge_gpio_cb_free(cookie: *mut c_void, line: u32) {
    cookie_as_backend(cookie).release(line)
}
