// SPDX-License-Identifier: GPL-2.0
//
// Hand-written extern "C" declarations for the Linux kernel C APIs
// the driver consumes. Pinned to Linux 6.13 LTS (driver/kernel-
// baseline/SUPPORT.md). See ffi/mod.rs for rationale.
//
// Naming follows C kernel convention verbatim so a `grep` for
// `i2c_add_adapter` lands on both the C kernel source and this file.

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

use core::ffi::{c_char, c_int, c_long, c_uint, c_ulong, c_void};

/* ───── Opaque kernel structs ──────────────────────────────────────── */

// We never construct these on the Rust side — only pass pointers.
// Representing them as ZSTs with `#[repr(C)]` gives us typed pointers
// without committing to field layout that C might change across
// kernel versions.

#[repr(C)] pub struct device        { _opaque: [u8; 0] }
#[repr(C)] pub struct usb_device    { _opaque: [u8; 0] }
#[repr(C)] pub struct usb_interface { _opaque: [u8; 0] }
#[repr(C)] pub struct urb           { _opaque: [u8; 0] }
#[repr(C)] pub struct usb_anchor    { _opaque: [u8; 0] }

#[repr(C)] pub struct auxiliary_device { _opaque: [u8; 0] }
#[repr(C)] pub struct auxiliary_driver { _opaque: [u8; 0] }

#[repr(C)] pub struct module           { _opaque: [u8; 0] }
#[repr(C)] pub struct lock_class_key   { _opaque: [u8; 0] }
#[repr(C)] pub struct wait_queue_head  { _opaque: [u8; 0] }

#[repr(C)] pub struct gpio_chip        { _opaque: [u8; 0] }
#[repr(C)] pub struct gpio_irq_chip    { _opaque: [u8; 0] }
#[repr(C)] pub struct irq_chip         { _opaque: [u8; 0] }

#[repr(C)] pub struct i2c_adapter      { _opaque: [u8; 0] }
#[repr(C)] pub struct i2c_msg          { _opaque: [u8; 0] }

#[repr(C)] pub struct spi_controller   { _opaque: [u8; 0] }
#[repr(C)] pub struct spi_device       { _opaque: [u8; 0] }
#[repr(C)] pub struct spi_message      { _opaque: [u8; 0] }
#[repr(C)] pub struct spi_transfer     { _opaque: [u8; 0] }

#[repr(C)] pub struct pwm_chip         { _opaque: [u8; 0] }
#[repr(C)] pub struct pwm_device       { _opaque: [u8; 0] }
#[repr(C)] pub struct pwm_state        { _opaque: [u8; 0] }
#[repr(C)] pub struct pwm_ops          { _opaque: [u8; 0] }

#[repr(C)] pub struct iio_dev          { _opaque: [u8; 0] }
#[repr(C)] pub struct iio_chan_spec    { _opaque: [u8; 0] }
#[repr(C)] pub struct iio_trigger      { _opaque: [u8; 0] }

/* ───── USB — include/linux/usb.h ──────────────────────────────────── */

// USB transfer direction flags (bEndpointAddress top bit).
pub const USB_DIR_OUT: u8 = 0x00;
pub const USB_DIR_IN:  u8 = 0x80;

// Endpoint types (bmAttributes low two bits).
pub const USB_ENDPOINT_XFERTYPE_MASK:      u8 = 0x03;
pub const USB_ENDPOINT_XFER_BULK:          u8 = 0x02;

extern "C" {
    /// Blocking bulk transfer. Returns 0 on success, -errno on failure.
    /// `actual_length` receives the number of bytes transferred.
    pub fn usb_bulk_msg(
        usb_dev:       *mut usb_device,
        pipe:          c_uint,
        data:          *mut c_void,
        len:           c_int,
        actual_length: *mut c_int,
        timeout:       c_int, /* milliseconds */
    ) -> c_int;

    /// Pipe encoders — these are static-inlines in usb.h; we re-declare
    /// them here as extern and provide the inline in a .c shim file
    /// (driver/src/ffi/usb_shim.c).
    pub fn rpbridge_usb_sndbulkpipe(dev: *mut usb_device, endpoint: c_uint) -> c_uint;
    pub fn rpbridge_usb_rcvbulkpipe(dev: *mut usb_device, endpoint: c_uint) -> c_uint;

    /// Returns the USB device bound to an interface.
    pub fn interface_to_usbdev(intf: *mut usb_interface) -> *mut usb_device;

    /// Endpoint iteration — walks `intf->cur_altsetting->endpoint[i]`.
    /// We read via our shim (driver/src/ffi/usb_shim.c) because the
    /// structure layouts are deeply private to the USB core.
    pub fn rpbridge_intf_num_endpoints(intf: *mut usb_interface) -> c_int;
    pub fn rpbridge_intf_endpoint_address(intf: *mut usb_interface, i: c_int) -> u8;
    pub fn rpbridge_intf_endpoint_attributes(intf: *mut usb_interface, i: c_int) -> u8;
    pub fn rpbridge_intf_endpoint_max_packet(intf: *mut usb_interface, i: c_int) -> u16;

    /// Returns the interface's own `struct device *`, for use as
    /// `parent` when registering subsystem objects (gpio_chip,
    /// i2c_adapter, …).
    pub fn rpbridge_intf_parent_dev(intf: *mut usb_interface) -> *mut device;
}

/* ───── auxiliary_bus — include/linux/auxiliary_bus.h ──────────────── */

extern "C" {
    pub fn auxiliary_device_init(adev: *mut auxiliary_device) -> c_int;
    pub fn __auxiliary_device_add(
        adev:    *mut auxiliary_device,
        modname: *const c_char,
    ) -> c_int;
    pub fn auxiliary_device_delete(adev: *mut auxiliary_device);
    pub fn auxiliary_device_uninit(adev: *mut auxiliary_device);
}

/* ───── wait_queue — include/linux/wait.h ──────────────────────────── */

// wait_queue_head_t has non-trivial layout (spinlock + list). We
// allocate storage via a fixed byte array sized for the 6.13 layout
// and let the kernel initialise it via init_waitqueue_head().
// `RPBRIDGE_WQ_HEAD_SIZE` is verified by a .c-side _Static_assert in
// the shim, so a kernel bump that changes the layout breaks the
// build rather than corrupting memory.
pub const RPBRIDGE_WQ_HEAD_SIZE: usize = 24; /* ~ 2 * sizeof(long) + ptr */

extern "C" {
    pub fn rpbridge_waitqueue_init(wq: *mut u8);
    /// Returns 0 on wake-up, > 0 remaining jiffies on condition met,
    /// < 0 on signal. Our wrapper treats non-zero as "woken"; the
    /// condition check happens in Rust under the inner lock.
    pub fn rpbridge_wait_event_timeout_ms(
        wq: *mut u8, timeout_ms: c_ulong,
    ) -> c_long;
    pub fn rpbridge_wake_up_all(wq: *mut u8);
}

/* ───── GPIO — include/linux/gpio/driver.h ─────────────────────────── */

// `GPIOCHIP_NAME_MAX` is 32 in 6.13; we pick a safe ceiling.
pub const RPBRIDGE_GPIO_NAME_MAX: usize = 32;

extern "C" {
    /// Shim-allocated gpio_chip with the vtable callbacks routed
    /// through `rpbridge_gpio_*` trampolines. Returns a pointer to the
    /// allocated chip or NULL on OOM.
    pub fn rpbridge_gpio_chip_new(
        parent:    *mut device,
        label:     *const c_char,
        base:      c_int,   /* -1 for dynamic */
        ngpio:     u16,
        cookie:    *mut c_void, /* boxed GpioState<T> from the driver */
    ) -> *mut gpio_chip;

    pub fn rpbridge_gpio_chip_free(chip: *mut gpio_chip);

    /// Register a previously-allocated gpio_chip. Wraps
    /// gpiochip_add_data(). Returns 0 / -errno.
    pub fn rpbridge_gpio_chip_register(chip: *mut gpio_chip) -> c_int;

    /// Unregister. Wraps gpiochip_remove().
    pub fn rpbridge_gpio_chip_unregister(chip: *mut gpio_chip);

    /// Install the nested irq_chip on top of the gpio_chip. Called
    /// post-register. The caller supplies the number of parent IRQs
    /// and a demuxer callback (invoked by our event pump).
    pub fn rpbridge_gpio_girq_add(
        chip: *mut gpio_chip,
    ) -> c_int;

    /// Dispatch one virtual IRQ from a parsed edge event into the
    /// chip's nested irq_chip. Used by the events-channel pump.
    pub fn rpbridge_gpio_handle_edge(chip: *mut gpio_chip, line: c_uint);
}

/* ───── I²C — include/linux/i2c.h ──────────────────────────────────── */

// i2c_msg.flags bits we care about.
pub const I2C_M_RD:       u16 = 0x0001;
pub const I2C_M_TEN:      u16 = 0x0010;
pub const I2C_M_NOSTART:  u16 = 0x4000;

// Functionality bits returned by .functionality().
pub const I2C_FUNC_I2C:            u32 = 0x00000001;
pub const I2C_FUNC_10BIT_ADDR:     u32 = 0x00000002;
pub const I2C_FUNC_SMBUS_EMUL:     u32 = 0x0eff_0008;

extern "C" {
    /// Allocate + initialise a kernel i2c_adapter with our master_xfer
    /// callback plumbed through a trampoline that invokes the Rust
    /// callback in `cookie`. Returns the adapter or NULL.
    pub fn rpbridge_i2c_adapter_new(
        parent: *mut device,
        name:   *const c_char,
        cookie: *mut c_void, /* boxed I2cState from the driver */
    ) -> *mut i2c_adapter;

    /// Register. Wraps i2c_add_adapter().
    pub fn rpbridge_i2c_adapter_register(adap: *mut i2c_adapter) -> c_int;

    /// Unregister + free. Wraps i2c_del_adapter() + kfree of the
    /// shim-allocated storage.
    pub fn rpbridge_i2c_adapter_free(adap: *mut i2c_adapter);

    /// i2c_msg field accessors — the struct layout is stable enough
    /// across 6.13..6.x that we could read directly, but going through
    /// the shim future-proofs against layout churn and keeps the
    /// bindings file free of #[repr(C)] mirror structs.
    pub fn rpbridge_i2c_msg_addr(msg: *mut i2c_msg) -> u16;
    pub fn rpbridge_i2c_msg_flags(msg: *mut i2c_msg) -> u16;
    pub fn rpbridge_i2c_msg_len(msg: *mut i2c_msg) -> u16;
    pub fn rpbridge_i2c_msg_buf(msg: *mut i2c_msg) -> *mut u8;
}

/* ───── SPI — include/linux/spi/spi.h ─────────────────────────────── */

// SPI mode bits (SPI_MODE_0..3 are combinations of CPOL | CPHA).
pub const SPI_CPHA: u32 = 0x01;
pub const SPI_CPOL: u32 = 0x02;

extern "C" {
    pub fn rpbridge_spi_controller_new(
        parent:        *mut device,
        num_chipselect: u16,
        cookie:        *mut c_void,
    ) -> *mut spi_controller;

    pub fn rpbridge_spi_controller_register(ctlr: *mut spi_controller) -> c_int;
    pub fn rpbridge_spi_controller_free(ctlr: *mut spi_controller);

    /// Transfer iteration accessors. A spi_message is a linked list of
    /// spi_transfer; we get head + next from the shim.
    pub fn rpbridge_spi_msg_first_xfer(msg: *mut spi_message) -> *mut spi_transfer;
    pub fn rpbridge_spi_msg_next_xfer(
        msg: *mut spi_message, cur: *mut spi_transfer,
    ) -> *mut spi_transfer;

    pub fn rpbridge_spi_xfer_tx_buf(x: *mut spi_transfer) -> *const u8;
    pub fn rpbridge_spi_xfer_rx_buf(x: *mut spi_transfer) -> *mut u8;
    pub fn rpbridge_spi_xfer_len(x: *mut spi_transfer) -> u32;
    pub fn rpbridge_spi_xfer_cs_change(x: *mut spi_transfer) -> bool;
    pub fn rpbridge_spi_dev_cs_pin(dev: *mut spi_device) -> u8;

    pub fn rpbridge_spi_finalize_current_message(ctlr: *mut spi_controller);
}

/* ───── PWM — include/linux/pwm.h ─────────────────────────────────── */

pub const PWM_POLARITY_NORMAL:   c_int = 0;
pub const PWM_POLARITY_INVERSED: c_int = 1;

extern "C" {
    pub fn rpbridge_pwm_chip_new(
        parent: *mut device,
        npwm:   c_uint,
        cookie: *mut c_void,
    ) -> *mut pwm_chip;

    pub fn rpbridge_pwm_chip_register(chip: *mut pwm_chip) -> c_int;
    pub fn rpbridge_pwm_chip_free(chip: *mut pwm_chip);

    pub fn rpbridge_pwm_state_period_ns(st: *mut pwm_state) -> u64;
    pub fn rpbridge_pwm_state_duty_ns(st: *mut pwm_state) -> u64;
    pub fn rpbridge_pwm_state_enabled(st: *mut pwm_state) -> bool;
    pub fn rpbridge_pwm_state_polarity(st: *mut pwm_state) -> c_int;

    pub fn rpbridge_pwm_state_set_period(st: *mut pwm_state, v: u64);
    pub fn rpbridge_pwm_state_set_duty(st: *mut pwm_state, v: u64);
    pub fn rpbridge_pwm_state_set_enabled(st: *mut pwm_state, v: bool);
    pub fn rpbridge_pwm_state_set_polarity(st: *mut pwm_state, v: c_int);

    pub fn rpbridge_pwm_dev_hwpwm(dev: *mut pwm_device) -> c_uint;
}

/* ───── IIO — include/linux/iio/iio.h ─────────────────────────────── */

pub const IIO_VOLTAGE:     c_int = 3;
pub const IIO_TEMP:        c_int = 9;

pub const IIO_CHAN_INFO_RAW:        c_int = 0;
pub const IIO_CHAN_INFO_SCALE:      c_int = 4;
pub const IIO_CHAN_INFO_PROCESSED:  c_int = 9;

pub const IIO_VAL_INT:          c_int = 1;
pub const IIO_VAL_INT_PLUS_MICRO: c_int = 2;
pub const IIO_VAL_FRACTIONAL:   c_int = 10;

extern "C" {
    pub fn rpbridge_iio_device_new(
        parent:      *mut device,
        num_channels: c_uint,
        cookie:      *mut c_void,
    ) -> *mut iio_dev;

    pub fn rpbridge_iio_device_register(indio: *mut iio_dev) -> c_int;
    pub fn rpbridge_iio_device_free(indio: *mut iio_dev);

    /// Allocate + configure a triggered buffer on the indio_dev. The
    /// trigger fires when core_driver's event pump calls
    /// `rpbridge_iio_push_sample_block`.
    pub fn rpbridge_iio_triggered_buffer_setup(indio: *mut iio_dev) -> c_int;
    pub fn rpbridge_iio_triggered_buffer_teardown(indio: *mut iio_dev);
    pub fn rpbridge_iio_push_sample_block(
        indio:    *mut iio_dev,
        samples:  *const u16,
        n_samples: c_uint,
        ts_ns:    u64,
    );
}

/* ───── Error codes — copy of include/uapi/asm-generic/errno.h ────── */
// Mirrored from asm-generic/errno-base.h + errno.h. Stable for decades.
pub const EPERM:     c_int =  1;
pub const ENOENT:    c_int =  2;
pub const EIO:       c_int =  5;
pub const EAGAIN:    c_int = 11;
pub const ENOMEM:    c_int = 12;
pub const EBUSY:     c_int = 16;
pub const ENODEV:    c_int = 19;
pub const EINVAL:    c_int = 22;
pub const ERANGE:    c_int = 34;
pub const EPROTO:    c_int = 71;
pub const EMSGSIZE:  c_int = 90;
pub const ENOTSUP:   c_int = 95;
pub const EOVERFLOW: c_int = 75;
pub const ETIMEDOUT: c_int = 110;
pub const ECANCELED: c_int = 125;
