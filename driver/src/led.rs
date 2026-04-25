// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-led — RPBP LED subsystem client (WS2812 / SK6812 / WS2811
// addressable strips). Wire format in docs/protocol/subsystems/led.md.
//
// Layer 1 (`wire`) builds the request bytes and parses replies. Layer 2
// (`Client<T>`) glues those builders to the round-trip transport. The
// kernel binding (Layer 3) is gated on the upstream
// `kernel::led::Multicolor` shape and is left as a placeholder until
// that landing — see driver/TODO.md §M12 for tracking.
//
// The design mirrors `iio.rs` and `pwm.rs`: every method is one
// round-trip on CHAN_CONTROL; the transport's RoundTrip trait owns the
// blocking/wakeup contract.

use kernel::prelude::*;
use kernel::sync::Arc;

use crate::rpbp::{CHAN_CONTROL, Status};
use crate::transport::RoundTrip;

/* ═════ Layer 1 — wire ════════════════════════════════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::led as ld};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Led as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 GET_INFO — empty args, returns the StripInfo struct. */
    pub fn build_get_info(dst: &mut [u8]) -> Result<usize, i32> {
        start(dst, ld::GET_INFO)
    }

    /// Layout matches the firmware reply: count u8 | pin u8 | format u8
    /// | reserved u8 | pixel_count LE u16 | max_pixels LE u16.
    pub struct StripInfo {
        pub count:        u8,
        pub pin:          u8,
        pub format:       u8,
        pub pixel_count:  u16,
        pub max_pixels:   u16,
    }

    pub fn parse_info(resp: &[u8]) -> Result<StripInfo, i32> {
        if resp.len() < 8 { return Err(crate::errno::EPROTO); }
        Ok(StripInfo {
            count:       resp[0],
            pin:         resp[1],
            format:      resp[2],
            pixel_count: rpbp::rd_le16(&resp[4..6]),
            max_pixels:  rpbp::rd_le16(&resp[6..8]),
        })
    }

    /* 0x01 CONFIGURE — [idx u8][format u8][pixel_count LE u16] */
    pub fn build_configure(
        dst: &mut [u8], idx: u8, format: u8, pixel_count: u16,
    ) -> Result<usize, i32> {
        let n = start(dst, ld::CONFIGURE)?;
        if dst.len() < n + 4 { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        dst[n + 1] = format;
        rpbp::wr_le16(&mut dst[n + 2..], pixel_count);
        Ok(n + 4)
    }

    /* 0x02 WRITE — [idx u8][offset LE u16][bytes …]
     *
     * `bytes` are pixel payload in the strip's configured format
     * (e.g. GRB = 3 bytes/pixel). Caller is responsible for ordering. */
    pub fn build_write(
        dst: &mut [u8], idx: u8, offset: u16, bytes: &[u8],
    ) -> Result<usize, i32> {
        let n = start(dst, ld::WRITE)?;
        if dst.len() < n + 3 + bytes.len() { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        rpbp::wr_le16(&mut dst[n + 1..], offset);
        dst[n + 3..n + 3 + bytes.len()].copy_from_slice(bytes);
        Ok(n + 3 + bytes.len())
    }

    /* 0x03 CLEAR — [idx u8] */
    pub fn build_clear(dst: &mut [u8], idx: u8) -> Result<usize, i32> {
        let n = start(dst, ld::CLEAR)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        Ok(n + 1)
    }
}

/* ═════ Layer 2 — Client ══════════════════════════════════════════ */

pub struct Client<T: RoundTrip + ?Sized> {
    rt: Arc<T>,
}

impl<T: RoundTrip + ?Sized> Client<T> {
    pub fn new(rt: Arc<T>) -> Self { Self { rt } }

    fn rt_call(&self, req: &[u8]) -> Result<KVec<u8>, i32> {
        let (status, payload) = self.rt.round_trip(CHAN_CONTROL, req)?;
        if !matches!(status, Status::Ok) { return Err(status.to_errno()); }
        Ok(payload)
    }

    pub fn info(&self) -> Result<wire::StripInfo, i32> {
        let mut buf = [0u8; 2];
        let n = wire::build_get_info(&mut buf)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_info(resp.as_slice())
    }

    pub fn configure(
        &self, idx: u8, format: u8, pixel_count: u16,
    ) -> Result<(), i32> {
        let mut buf = [0u8; 6];
        let n = wire::build_configure(&mut buf, idx, format, pixel_count)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    /// Write `bytes` of pixel payload starting at byte `offset` in the
    /// configured-format framebuffer. The firmware shifts the entire
    /// buffer to the strip after the partial update + holds the latch.
    ///
    /// `bytes.len()` must be ≤ 3 KiB (RPBP MTU minus the 5-byte
    /// header). Larger frames need to be split across multiple
    /// `write()` calls — the strip's framebuffer persists between
    /// calls so partial updates compose correctly.
    pub fn write(
        &self, idx: u8, offset: u16, bytes: &[u8],
    ) -> Result<(), i32> {
        // 16-byte stack scratch + payload — compute the upper bound.
        if bytes.len() > crate::rpbp::MAX_PAYLOAD_LEN.saturating_sub(8) {
            return Err(crate::errno::EINVAL);
        }
        let mut buf = KVec::new();
        buf.resize(5 + bytes.len(), 0u8, GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        let n = wire::build_write(buf.as_mut_slice(), idx, offset, bytes)?;
        let _ = self.rt_call(&buf.as_slice()[..n])?;
        Ok(())
    }

    pub fn clear(&self, idx: u8) -> Result<(), i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_clear(&mut buf, idx)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }
}

/* ═════ Layer 3 — child probe (placeholder) ═══════════════════════ *
 *
 * No kernel-side binding exists yet for addressable LED strips —
 * `led_classdev_multicolor` is upstream but lacks a Rust wrapper.
 * Until that lands, the child probe just constructs a Client and
 * pings GET_INFO so we surface a clear log line; userspace can talk
 * to the firmware via libusb in the meantime. Tracked in
 * driver/TODO.md §M12.
 */

use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;

pub(crate) struct LedChild {
    #[allow(dead_code)]
    client: Client<RpbridgeState>,
    #[allow(dead_code)]
    idx:    u16,
}

impl LedChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state.clone());
        match client.info() {
            Ok(info) => pr_info!(
                "rpbridge-led.{}: pin={} pixels={} max={} (no kernel binding yet)\n",
                bus.idx, info.pin, info.pixel_count, info.max_pixels),
            Err(e) => pr_warn!(
                "rpbridge-led.{}: GET_INFO failed: {}\n", bus.idx, e),
        }
        Ok(LedChild { client, idx: bus.idx })
    }
}

/* ═════ Tests ═════════════════════════════════════════════════════ */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpbp::op::led as ld;

    #[test]
    fn build_get_info_layout() {
        let mut buf = [0u8; 2];
        let n = wire::build_get_info(&mut buf).unwrap();
        assert_eq!(n, 2);
        assert_eq!(buf, [0x07, ld::GET_INFO]);
    }

    #[test]
    fn parse_info_extracts_all_fields() {
        let resp: [u8; 8] = [
            1,        // count
            43,       // pin
            ld::FMT_GRB,
            0,        // reserved
            0x10, 0x00,    // pixel_count = 16
            0x00, 0x01,    // max_pixels = 256
        ];
        let info = wire::parse_info(&resp).unwrap();
        assert_eq!(info.count, 1);
        assert_eq!(info.pin, 43);
        assert_eq!(info.format, ld::FMT_GRB);
        assert_eq!(info.pixel_count, 16);
        assert_eq!(info.max_pixels, 256);
    }

    #[test]
    fn build_configure_layout() {
        let mut buf = [0u8; 6];
        let n = wire::build_configure(&mut buf, 0, ld::FMT_GRBW, 64).unwrap();
        assert_eq!(n, 6);
        assert_eq!(buf, [0x07, ld::CONFIGURE, 0, ld::FMT_GRBW, 0x40, 0x00]);
    }

    #[test]
    fn build_write_carries_payload() {
        let mut buf = [0u8; 16];
        let pixels: [u8; 6] = [0x10, 0x20, 0x30, 0x40, 0x50, 0x60];
        let n = wire::build_write(&mut buf, 0, 3, &pixels).unwrap();
        assert_eq!(n, 11);
        // header + idx + offset
        assert_eq!(&buf[..5], &[0x07, ld::WRITE, 0, 0x03, 0x00]);
        // payload bytes verbatim
        assert_eq!(&buf[5..11], &pixels);
    }

    #[test]
    fn build_clear_layout() {
        let mut buf = [0u8; 3];
        let n = wire::build_clear(&mut buf, 0).unwrap();
        assert_eq!(n, 3);
        assert_eq!(buf, [0x07, ld::CLEAR, 0]);
    }
}

#[cfg(test)]
mod client_tests {
    use super::*;
    use crate::transport::MockRoundTrip;

    fn setup() -> (Client<MockRoundTrip>, Arc<MockRoundTrip>) {
        let mock = Arc::new(MockRoundTrip::new());
        let client = Client::new(mock.clone());
        (client, mock)
    }

    #[test]
    fn info_round_trip() {
        let (client, mock) = setup();
        let body: [u8; 8] = [1, 14, 0, 0, 0x08, 0x00, 0x00, 0x01];
        mock.queue_response(Status::Ok, &body);
        let info = client.info().unwrap();
        assert_eq!(info.pin, 14);
        assert_eq!(info.pixel_count, 8);
        assert_eq!(info.max_pixels, 256);
    }

    #[test]
    fn configure_propagates_einval_status() {
        let (client, mock) = setup();
        mock.queue_response(Status::EInval, &[]);
        let err = client.configure(0, 99, 1).unwrap_err();
        assert_eq!(err, crate::errno::EINVAL);
    }

    #[test]
    fn write_emits_correct_bytes() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        let pixels: [u8; 3] = [0xFF, 0x00, 0x00];   // GRB: green only
        client.write(0, 0, &pixels).unwrap();
        let req = mock.pop_request();
        let s = req.as_slice();
        assert_eq!(s[..5], [0x07, 0x02, 0, 0x00, 0x00]);
        assert_eq!(&s[5..8], &pixels);
    }

    #[test]
    fn write_rejects_oversize_payload() {
        let (client, _mock) = setup();
        // Construct a payload that, after the 5-byte WRITE header,
        // would exceed RPBP's 4 KiB MTU. The Client must reject
        // before even hitting the transport.
        let big = [0u8; 5000];
        let err = client.write(0, 0, &big).unwrap_err();
        assert_eq!(err, crate::errno::EINVAL);
    }

    #[test]
    fn clear_one_round_trip() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        client.clear(0).unwrap();
        assert_eq!(mock.request_count(), 1);
        assert_eq!(mock.pop_request().as_slice(), &[0x07, 0x03, 0]);
    }
}
