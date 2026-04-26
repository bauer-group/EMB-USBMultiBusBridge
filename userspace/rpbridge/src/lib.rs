// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// rpbridge — high-level client library.
//
// Two goals:
//   1. Hide the USB transport so callers write `dev.i2c(0).read(0x76, 6)`
//      instead of shuffling bulk URBs.
//   2. Present a typed view of each subsystem (I²C, SPI, GPIO, PWM, ADC,
//      UART phy config) so misuse is a compile-time error, not a
//      runtime surprise.
//
// The crate is deliberately transport-agnostic: swap `nusb` for `rusb`
// at build time via Cargo features. The rest of the API stays the same.

#![warn(clippy::pedantic)]
#![allow(clippy::module_name_repetitions)]

use thiserror::Error;

pub use rpbridge_protocol as protocol;

/// Errors raised by the high-level client.
#[derive(Debug, Error)]
pub enum Error {
    /// Low-level USB transport failure.
    #[error("usb transport error: {0}")]
    Transport(String),
    /// Protocol-layer error (framing, CRC, …).
    #[error("protocol error: {0}")]
    Protocol(#[from] protocol::Error),
    /// The device answered with a non-`Ok` status code.
    #[error("device status {0:?}")]
    DeviceStatus(protocol::Status),
    /// Capability map did not advertise the requested subsystem.
    #[error("feature '{0}' not advertised by the device")]
    FeatureNotAvailable(&'static str),
    /// Operation timed out at the host before the device replied.
    #[error("operation timed out after {0} ms")]
    Timeout(u32),
}

/// Short-hand result alias.
pub type Result<T> = core::result::Result<T, Error>;

/// `RPBridge`-specific USB VID/PID placeholder. Replace with the real
/// allocation before the first public release.
pub const DEFAULT_VID: u16 = 0x1209;
/// `RPBridge`-specific USB VID/PID placeholder. Replace with the real
/// allocation before the first public release.
pub const DEFAULT_PID: u16 = 0xFFFE;

/// Handle to a connected `RPBridge` device.
pub struct Device {
    // TODO: box a transport trait object once the `nusb` pipe helpers
    //       settle their async API in 0.2.
    _phantom: core::marker::PhantomData<()>,
}

impl Device {
    /// Open the first `RPBridge` device currently plugged in.
    ///
    /// # Errors
    ///
    /// Returns `Error::Transport` if no matching device is found or if
    /// claiming the vendor interface fails.
    pub fn open_first() -> Result<Self> {
        // TODO §P3 — enumerate via nusb, filter by VID/PID, claim the
        // vendor interface, read the capability map.
        Err(Error::Transport(
            "Device::open_first() is not implemented yet — see docs/protocol/rpbp-v1.md"
                .to_string(),
        ))
    }

    /// Return a copy of the capability map retrieved at open time.
    #[must_use]
    pub fn capabilities(&self) -> protocol::cap::Capabilities {
        protocol::cap::Capabilities::default()
    }
}
