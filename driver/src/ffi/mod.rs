// SPDX-License-Identifier: GPL-2.0
//
// FFI bindings to the C kernel APIs that Rust-for-Linux has NOT yet
// provided safe wrappers for. Every submodule exposes a narrow
// `unsafe`-encapsulating safe API that the rest of the driver consumes.
//
// ## Why this exists
//
// As of Linux 6.18 the R4L `kernel` crate exposes `kernel::usb::Driver`
// (probe / disconnect / match tables) but NOT `usb_bulk_msg`, URB
// submission, auxiliary_bus registration, or any of the subsystem
// registration APIs (gpio_chip, i2c_adapter, spi_controller, pwm_chip,
// iio_device). Without those we cannot deliver a working driver.
//
// Rather than wait for upstream, we hand-wrap the five C API families
// we need. Every `extern "C"` declaration is paired with a safe Rust
// wrapper plus a `// SAFETY:` comment documenting the invariants the
// caller must uphold. The unsafe code is confined to `ffi/`; the rest
// of the driver is safe Rust.
//
// ## Precedent
//
// This is the same pattern the Nova GPU driver, the Binder-rust
// driver, and the Block-rust driver all used during their transition
// periods. When the upstream `kernel::{usb,i2c,spi,gpio,pwm,iio}`
// modules grow the functionality we need, individual `ffi/*.rs` files
// will be deleted and their call sites rewired to the safe upstream
// API — that is expressly why every safe wrapper here mirrors the
// shape of the eventual upstream API.
//
// ## Kernel ABI stability
//
// The declarations in `bindings.rs` are pinned to Linux 6.13 LTS, our
// minimum-supported kernel per `driver/kernel-baseline/SUPPORT.md`.
// They are compatible with 6.14, 6.15, 6.18 — only additive kernel
// changes are in-scope for the matrix. Any breaking ABI change in a
// used function is a CI failure via the `nightly-mainline` workflow.
//
// ## How to regenerate
//
// These declarations are hand-written rather than `bindgen`-generated
// to keep the build system independent of libclang / kernel-header
// availability on the developer's host. The function signatures have
// been cross-checked against:
//
//   include/linux/usb.h                (Linux 6.13)
//   include/linux/auxiliary_bus.h
//   include/linux/gpio/driver.h
//   include/linux/i2c.h
//   include/linux/spi/spi.h
//   include/linux/pwm.h
//   include/linux/iio/iio.h
//   include/linux/wait.h
//
// If a future kernel version changes a signature we use, the CI will
// fail at link time. The corresponding declaration gets amended here.

#![allow(dead_code)]

pub mod bindings;

pub mod usb;
pub mod auxiliary;
pub mod wait;
pub mod gpio;
pub mod i2c;
pub mod spi;
pub mod pwm;
pub mod iio;
