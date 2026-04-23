// SPDX-License-Identifier: GPL-2.0
//
// RPBridge — USB interface driver entry point.
//
// Lifecycle:
//
//   probe()     USB interface matched. Allocate per-device state, submit
//               the first bulk-IN URB, query the CBOR capability map via
//               a synchronous RPBP CMD_REQUEST + CMD_RESPONSE, then spawn
//               one auxiliary_device per advertised subsystem.
//
//   disconnect() Tear down in reverse: unregister auxiliary_devices,
//                kill anchored URBs, drop per-device state. The kernel
//                USB core guarantees no further callbacks once
//                usb_kill_anchored_urbs() has returned.
//
// This file is intentionally small — most of the RPBP logic lives in
// `crate::rpbp`, per-subsystem logic lives in the respective child
// modules, and CBOR parsing lives in `crate::cap`.

use kernel::prelude::*;
use kernel::sync::{Arc, Mutex};

use crate::cap::Capabilities;
use crate::rpbp;

/// Empty shim for the in-kernel CRC-32C. When the `kernel::crc32c`
/// binding is present (recent Rust-for-Linux trees expose it via
/// `rust/kernel/crc32c.rs`), replace this body with a call into that
/// module. Until then, an inline software implementation keeps the
/// driver self-contained and byte-identical with the firmware.
///
/// Table and algorithm are slice-by-1 over the Castagnoli polynomial
/// 0x1EDC6F41 (reflected 0x82F63B78).
pub(crate) fn crc32c_slice(buf: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in buf {
        let idx = ((crc ^ u32::from(b)) & 0xFFu32) as usize;
        crc = (crc >> 8) ^ CRC32C_TABLE[idx];
    }
    crc ^ 0xFFFF_FFFF
}

const CRC32C_TABLE: [u32; 256] = {
    // Build the table in a const-fn context. `rustc` unrolls the loop
    // at compile time so the resulting table is stored as data.
    let mut t = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut c: u32 = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if (c & 1) != 0 {
                (c >> 1) ^ 0x82F63B78
            } else {
                c >> 1
            };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
};

/// Per-device state held in an `Arc` so auxiliary_device children can
/// reference it without lifetime acrobatics.
#[allow(dead_code)]
pub(crate) struct RpbridgeState {
    /// Parsed capability map — the only place that holds it.
    pub(crate) caps: Mutex<Option<Capabilities>>,
    // TODO: URB anchor, bulk-in/out endpoint handles, pending
    //       completion table indexed by RPBP sequence number, CBOR
    //       arena, attestation nonce, etc.
}

/// The USB driver glue.
pub(crate) struct RpbridgeDriver;

// NOTE: the exact trait + impl shape depends on the Rust-for-Linux USB
// bindings available in the target kernel. The skeleton below uses the
// shape that landed with the `kernel::usb` abstraction; adapt the impl
// trait names if you target a kernel with a different set of bindings.
//
// Match table: bind to our VID/PID, any interface (we pick the vendor
// one by class/subclass inside probe()).

#[allow(dead_code)]
const RPBRIDGE_USB_VID: u16 = 0x1209; // pid.codes (change to allocated PID)
#[allow(dead_code)]
const RPBRIDGE_USB_PID: u16 = 0xFFFE; // pid.codes testing range

#[vtable]
impl kernel::usb::Driver for RpbridgeDriver {
    type Data = Arc<RpbridgeState>;
    type IdInfo = ();

    const ID_TABLE: kernel::usb::IdTable<Self::IdInfo> =
        &kernel::usb::IdTable::new(&[
            // Match any interface of the composite device — we filter on
            // class/subclass inside probe().
            kernel::usb::DeviceId::new(RPBRIDGE_USB_VID, RPBRIDGE_USB_PID),
        ]);

    fn probe(
        _intf: &kernel::usb::Interface,
        _id_info: Option<&Self::IdInfo>,
    ) -> Result<Arc<RpbridgeState>> {
        pr_info!("rpbridge: probing a matched USB interface\n");

        let state = Arc::try_new(RpbridgeState {
            caps: Mutex::new(None),
        })?;

        // TODO §M1:
        //   - discover bulk endpoints on this interface, reject if not
        //     the vendor/RPBP pair
        //   - allocate and anchor a pool of bulk-IN URBs
        //   - send HELLO and fetch CAPABILITIES (CBOR)
        //   - parse via crate::cap::Capabilities::from_cbor()
        //   - for each advertised subsystem, spawn an auxiliary_device
        //     under a name like "rpbridge-gpio", "rpbridge-i2c", etc.

        Ok(state)
    }

    fn disconnect(_intf: &kernel::usb::Interface, _data: Arc<RpbridgeState>) {
        pr_info!("rpbridge: interface disconnected\n");
        // TODO:
        //   - usb_kill_anchored_urbs() on our URB anchor
        //   - auxiliary_device_unregister() all children
        //   - drop per-device state via Arc
    }

    fn suspend(_intf: &kernel::usb::Interface, _msg: kernel::usb::Message)
        -> Result
    {
        Ok(())
    }

    fn resume(_intf: &kernel::usb::Interface) -> Result {
        Ok(())
    }
}

// Sanity checks — these tests run on the host when building with the
// out-of-tree user-mode Linux kunit flow.
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32c_empty_equals_zero() {
        assert_eq!(crc32c_slice(&[]), 0);
    }

    #[test]
    fn crc32c_known_vector() {
        // CRC-32C of ASCII "123456789" is 0xE3069283 per
        // the RFC 3720 test vector.
        assert_eq!(crc32c_slice(b"123456789"), 0xE306_9283);
    }

    #[test]
    fn header_roundtrip() {
        let h = rpbp::Header {
            magic: rpbp::MAGIC,
            version: rpbp::VERSION,
            msg_type: rpbp::MsgType::Ping as u8,
            flags: rpbp::Flags::CBOR,
            channel: 0,
            seq: 42,
            payload_len: 0,
            timestamp_us: 0x1234_5678,
        };
        let mut buf = [0u8; rpbp::HEADER_LEN];
        h.encode(&mut buf);
        let parsed = rpbp::Header::parse(&buf).unwrap();
        assert_eq!(parsed.seq, 42);
        assert_eq!(parsed.timestamp_us, 0x1234_5678);
    }
}
