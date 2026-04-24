// SPDX-License-Identifier: GPL-2.0
//
// Parent-device resolution + future home of the auxiliary_bus
// wrapper.
//
// The first driver cut registers each subsystem (gpio_chip,
// i2c_adapter, spi_controller, pwm_chip, iio_dev) directly as a
// child of the USB interface's `struct device`. This works, passes
// every functional test, lands the subsystems in the expected
// /sys/bus/usb/devices/.../i2c-N / spidev* / gpiochip* / pwmchip* /
// iio:deviceN paths that libgpiod / i2c-tools / iio-utils / pwm-utils
// already know how to find.
//
// **Refactor planned.** ADR-0002 chose `auxiliary_bus` as the
// structural pattern for eventual upstream submission. Moving to
// aux_bus is mechanical:
//
//   1. Each Child grows an `auxiliary_device_{init,register,delete,
//      uninit}` pair.
//   2. The inner subsystem object (gpio_chip etc.) attaches to
//      aux_device->dev instead of the USB interface's device.
//   3. A matching `auxiliary_driver` with match table is registered
//      in `RpbridgeDriver::new()`.
//
// Tracked as M12 in driver/TODO.md. Does not block functional
// verification.

use core::ptr::NonNull;

use crate::ffi::{self, bindings as cc};

/// Resolve the parent `struct device *` that a subsystem Child
/// registers under. Called from each Child's `probe()` after it has
/// a clone of the Arc<RpbridgeState>.
pub fn parent_device(state: &crate::core_driver::RpbridgeState)
    -> Result<NonNull<cc::device>, i32>
{
    let g = state.transport.lock();
    let intf = g.intf.as_ref().ok_or(cc::ENODEV)?;
    // SAFETY: `intf` lives for the duration of the probe → disconnect
    // window; the shim returns a stable `&intf->dev`.
    let dev = unsafe { cc::rpbridge_intf_parent_dev(intf.as_ptr()) };
    NonNull::new(dev).ok_or(cc::ENODEV)
}

/// Convenience alias for the Child constructors.
pub type ParentDev = NonNull<cc::device>;

// Re-export bindings pointer types for call-site ergonomics.
pub use cc::auxiliary_device;

// Touch ffi so the module dependency graph exposes shared types.
pub(crate) fn _ffi_touch() { let _: Option<&ffi::usb::Interface> = None; }
