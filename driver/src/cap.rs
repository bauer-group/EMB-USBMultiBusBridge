// SPDX-License-Identifier: GPL-2.0
//
// Capability-map parser. The device answers a CMD_REQUEST{GET_CAPABILITIES}
// with a CBOR map described in docs/drafts/rpbridge-driver-stack.md §4.9
// (and, once written, docs/protocol/rpbp-v1.md §capability). Fields
// are discovered at runtime — we do not hard-code a schema version in
// the driver.
//
// Kernel-side CBOR parsing: we consume a minimal subset of RFC 8949
// (maps of text-string keys to primitive/array/map values) via a
// hand-rolled parser in this file. A full CBOR library would pull in
// too much code for a kernel module and we do not need it — the
// protocol caps the capability blob at 4 kB and defines a narrow set
// of types.

use kernel::prelude::*;

/// Per-bus descriptor extracted from the capability map.
#[allow(dead_code)]
#[derive(Debug, Default, Clone)]
pub(crate) struct BusCaps {
    pub class: &'static str, // "i2c", "spi", "gpio", "pwm", "adc", "uart"
    pub instance: u16,
    pub channel: u16,
    pub pin_count: u16,
    pub max_freq_hz: u32,
}

#[allow(dead_code)]
#[derive(Debug, Default)]
pub(crate) struct Capabilities {
    pub proto: (u8, u8, u8),
    pub fw_version: KString,
    pub board: KString,
    pub mtu_out: u16,
    pub mtu_in: u16,
    pub buses: Vec<BusCaps>,
}

#[allow(dead_code)]
impl Capabilities {
    /// Parse a CBOR byte slice into a `Capabilities` value. Returns
    /// `-EPROTO` on any structural violation. The parser is
    /// deliberately strict: unknown keys inside the top-level map are
    /// tolerated (forward compatibility), but malformed CBOR is not.
    pub fn from_cbor(_src: &[u8]) -> Result<Self, i32> {
        // TODO §M1: implement a small CBOR walker. See RFC 8949 §3
        // for major-type encoding and the feature list in the driver-
        // stack draft §4.9 for the exact key names to recognize.
        //
        // Until then, return a minimal stub that satisfies the caller.
        Ok(Capabilities::default())
    }
}
