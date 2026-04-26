// SPDX-License-Identifier: GPL-2.0
//
// RPBridge — Rust-for-Linux USB multi-bus bridge driver.
//
// This module binds to the RPBridge composite USB device and spawns a
// set of auxiliary devices, one per subsystem the device advertises in
// its CBOR capability map (I²C, SPI, GPIO, IIO/ADC, PWM, …). CAN is
// handled by the mainline gs_usb driver through a separate vendor
// interface on the same device, so no CAN code lives in this crate.
//
// Subsystem map:
//
//     kernel::usb::Driver       -->  RpbridgeDriver (this crate)
//                                    |
//                                    v
//     auxiliary::Device       -->  rpbridge-gpio.<N>
//     auxiliary::Device       -->  rpbridge-i2c.<N>
//     auxiliary::Device       -->  rpbridge-spi.<N>
//     auxiliary::Device       -->  rpbridge-pwm.<N>
//     auxiliary::Device       -->  rpbridge-iio.<N>
//
// Each auxiliary child is matched by its own in-tree auxiliary driver
// (defined in the child modules of this crate), which then registers
// into the appropriate subsystem core (`gpio_chip`, `i2c_adapter`,
// `spi_controller`, `pwm_chip`, `iio_dev`).

#![no_std]
#![allow(clippy::new_without_default)]

use kernel::prelude::*;

mod rpbp;
mod cap;
mod transport;
mod ffi;
mod core_driver;

mod gpio;
mod i2c;
mod spi;
mod pwm;
mod iio;
mod led;
mod dmx;
mod onewire;
mod i2s;

/// Generic errors this driver maps to/from kernel `Result<T>`.
///
/// The kernel binding's `kernel::error::*` constants are the canonical
/// source for these values, but they ship as `kernel::error::Error`
/// types (not bare `i32`). The wire-protocol layer needs to compare
/// against numeric errno codes, so we mirror the small subset we use
/// as plain `i32` constants and keep the values in lock-step with
/// `<linux/errno.h>` (also matched by glibc/musl headers).
#[allow(dead_code)]
pub(crate) mod errno {
    pub const EPERM:      i32 = 1;
    pub const ENOENT:     i32 = 2;
    pub const EIO:        i32 = 5;
    pub const EAGAIN:     i32 = 11;
    pub const ENOMEM:     i32 = 12;
    pub const EBUSY:      i32 = 16;
    pub const ENODEV:     i32 = 19;
    pub const EINVAL:     i32 = 22;
    pub const EPROTO:     i32 = 71;
    pub const EOVERFLOW:  i32 = 75;
    pub const EMSGSIZE:   i32 = 90;
    pub const ENOTSUP:    i32 = 95;
    pub const ETIMEDOUT:  i32 = 110;
    pub const ECANCELED:  i32 = 125;
}

kernel::module_usb_driver! {
    type:    core_driver::RpbridgeDriver,
    name:    "rpbridge",
    authors: ["BAUER GROUP <kb@de.bauer-group.com>"],
    description: "RPBridge USB multi-bus bridge — RPBP v1 transport \
                  + auxiliary_bus child spawns per subsystem",
    license: "GPL v2",
    alias:   ["usb:rpbridge"],
    params:  {
        // 7.0's params! only accepts `default` + `description`. The
        // module_param permission field is no longer user-controlled
        // — kernel R4L picked 0o644 by convention. Reflected: the
        // sysfs entry at /sys/module/rpbridge/parameters/debug_dump
        // is owner-writable, world-readable.
        debug_dump: bool {
            default: false,
            description: "Dump received RPBP frames to dmesg (debug only).",
        },
    },
}
