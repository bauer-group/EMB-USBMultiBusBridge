// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-i2s — RPBP I²S TX client. Wire format in
// docs/protocol/subsystems/i2s.md.
//
// Linux audio integration goes through ALSA / ASoC, which is a
// substantially heavier mechanism than the auxiliary_bus + class
// device pattern other RPBP subsystems use. v1 ships only the wire
// + Client layers; the ASoC machine driver is tracked in
// driver/TODO.md §M12 as a follow-up. Userspace can drive the
// firmware directly via libusb in the meantime — useful for system
// sounds, beeps, and test tones.

use kernel::prelude::*;
use kernel::sync::Arc;

use crate::rpbp::{CHAN_CONTROL, Status};
use crate::transport::RoundTrip;

/* ═════ Layer 1 — wire ════════════════════════════════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::i2s as i2};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::I2s as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 GET_INFO — empty args */
    pub fn build_get_info(dst: &mut [u8]) -> Result<usize, i32> {
        start(dst, i2::GET_INFO)
    }

    pub struct I2sInfo {
        pub count:          u8,
        pub bclk_pin:       u8,
        pub lrclk_pin:      u8,
        pub dout_pin:       u8,
        pub sample_rate_hz: u32,
        pub bit_depth:      u8,
        pub running:        bool,
    }

    pub fn parse_info(resp: &[u8]) -> Result<I2sInfo, i32> {
        if resp.len() < 12 { return Err(crate::errno::EPROTO); }
        Ok(I2sInfo {
            count:          resp[0],
            bclk_pin:       resp[1],
            lrclk_pin:      resp[2],
            dout_pin:       resp[3],
            sample_rate_hz: rpbp::rd_le32(&resp[4..8]),
            bit_depth:      resp[8],
            running:        resp[9] != 0,
        })
    }

    /* 0x01 CONFIGURE — [idx u8][sample_rate LE u32] */
    pub fn build_configure(
        dst: &mut [u8], idx: u8, sample_rate_hz: u32,
    ) -> Result<usize, i32> {
        let n = start(dst, i2::CONFIGURE)?;
        if dst.len() < n + 5 { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        rpbp::wr_le32(&mut dst[n + 1..], sample_rate_hz);
        Ok(n + 5)
    }

    /* 0x02 / 0x03 START / STOP — [idx u8] */
    pub fn build_start_stop(
        dst: &mut [u8], idx: u8, start_: bool,
    ) -> Result<usize, i32> {
        let op = if start_ { i2::START } else { i2::STOP };
        let n = start(dst, op)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        Ok(n + 1)
    }

    /* 0x04 WRITE — [idx u8][stereo PCM frames …]
     *
     * Each frame is 4 bytes: left u16 LE, right u16 LE. Caller's
     * responsibility to interleave correctly. */
    pub fn build_write(
        dst: &mut [u8], idx: u8, frames: &[u8],
    ) -> Result<usize, i32> {
        if (frames.len() & 0x3) != 0 { return Err(crate::errno::EINVAL); }
        let n = start(dst, i2::WRITE)?;
        if dst.len() < n + 1 + frames.len() { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        dst[n + 1..n + 1 + frames.len()].copy_from_slice(frames);
        Ok(n + 1 + frames.len())
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

    pub fn info(&self) -> Result<wire::I2sInfo, i32> {
        let mut buf = [0u8; 2];
        let n = wire::build_get_info(&mut buf)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_info(resp.as_slice())
    }

    pub fn configure(&self, idx: u8, sample_rate_hz: u32) -> Result<(), i32> {
        let mut buf = [0u8; 7];
        let n = wire::build_configure(&mut buf, idx, sample_rate_hz)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn start(&self, idx: u8) -> Result<(), i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_start_stop(&mut buf, idx, true)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn stop(&self, idx: u8) -> Result<(), i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_start_stop(&mut buf, idx, false)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    /// Push stereo PCM frames. `frames` must be a multiple of 4
    /// bytes (one frame = left u16 LE + right u16 LE). The firmware
    /// blocks until the FIFO accepts the entire buffer; from the
    /// caller's perspective the write returns "queued, not yet
    /// played" — pair with `info().running` to gate transitions.
    pub fn write(&self, idx: u8, frames: &[u8]) -> Result<(), i32> {
        if (frames.len() & 0x3) != 0 || frames.is_empty() {
            return Err(crate::errno::EINVAL);
        }
        if frames.len() > crate::rpbp::MAX_PAYLOAD_LEN.saturating_sub(8) {
            return Err(crate::errno::EINVAL);
        }
        let mut buf = KVec::new();
        buf.resize(3 + frames.len(), 0u8, GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        let n = wire::build_write(buf.as_mut_slice(), idx, frames)?;
        let _ = self.rt_call(&buf.as_slice()[..n])?;
        Ok(())
    }
}

/* ═════ Layer 3 — child probe (placeholder) ═══════════════════════ */

use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;

pub(crate) struct I2sChild {
    #[allow(dead_code)]
    client: Client<RpbridgeState>,
    #[allow(dead_code)]
    idx:    u16,
}

impl I2sChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state.clone());
        match client.info() {
            Ok(info) => pr_info!(
                "rpbridge-i2s.{}: bclk={} lrclk={} dout={} rate={} (no kernel binding yet)\n",
                bus.idx, info.bclk_pin, info.lrclk_pin,
                info.dout_pin, info.sample_rate_hz),
            Err(e) => pr_warn!(
                "rpbridge-i2s.{}: GET_INFO failed: {}\n", bus.idx, e),
        }
        Ok(I2sChild { client, idx: bus.idx })
    }
}

/* ═════ Tests ═════════════════════════════════════════════════════ */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpbp::op::i2s as i2;

    #[test]
    fn build_get_info_layout() {
        let mut buf = [0u8; 2];
        let n = wire::build_get_info(&mut buf).unwrap();
        assert_eq!(n, 2);
        assert_eq!(buf, [0x0A, i2::GET_INFO]);
    }

    #[test]
    fn parse_info_extracts_all_fields() {
        let resp: [u8; 12] = [
            1,           // count
            41, 42, 45,  // bclk, lrclk, dout
            0x80, 0xBB, 0x00, 0x00,   // sample_rate = 48000
            16,          // bit_depth
            0,           // running
            0, 0,        // reserved
        ];
        let info = wire::parse_info(&resp).unwrap();
        assert_eq!(info.count, 1);
        assert_eq!(info.bclk_pin, 41);
        assert_eq!(info.lrclk_pin, 42);
        assert_eq!(info.dout_pin, 45);
        assert_eq!(info.sample_rate_hz, 48_000);
        assert_eq!(info.bit_depth, 16);
        assert!(!info.running);
    }

    #[test]
    fn build_configure_layout() {
        let mut buf = [0u8; 7];
        let n = wire::build_configure(&mut buf, 0, 96_000).unwrap();
        assert_eq!(n, 7);
        assert_eq!(&buf[..3], &[0x0A, i2::CONFIGURE, 0]);
        // 96_000 = 0x0001_7700 → LE: 00 77 01 00
        assert_eq!(&buf[3..7], &[0x00, 0x77, 0x01, 0x00]);
    }

    #[test]
    fn build_start_stop_layout() {
        let mut buf = [0u8; 3];
        let n = wire::build_start_stop(&mut buf, 0, true).unwrap();
        assert_eq!(n, 3);
        assert_eq!(buf, [0x0A, i2::START, 0]);
        let n = wire::build_start_stop(&mut buf, 0, false).unwrap();
        assert_eq!(n, 3);
        assert_eq!(buf, [0x0A, i2::STOP, 0]);
    }

    #[test]
    fn build_write_carries_payload() {
        let mut buf = [0u8; 16];
        // Two stereo frames: (0x1234, 0x5678) and (0xAABB, 0xCCDD)
        let pcm: [u8; 8] = [
            0x34, 0x12, 0x78, 0x56,
            0xBB, 0xAA, 0xDD, 0xCC,
        ];
        let n = wire::build_write(&mut buf, 0, &pcm).unwrap();
        assert_eq!(n, 11);
        assert_eq!(&buf[..3], &[0x0A, i2::WRITE, 0]);
        assert_eq!(&buf[3..11], &pcm);
    }

    #[test]
    fn build_write_rejects_unaligned_frames() {
        let mut buf = [0u8; 16];
        // 3-byte payload is not a complete stereo frame.
        let err = wire::build_write(&mut buf, 0, &[1, 2, 3]).unwrap_err();
        assert_eq!(err, crate::errno::EINVAL);
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
    fn configure_round_trip_emits_rate() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        client.configure(0, 44_100).unwrap();
        let req = mock.pop_request();
        let s = req.as_slice();
        assert_eq!(&s[..3], &[0x0A, 0x01, 0]);
        // 44_100 = 0xAC44 → LE: 44 AC 00 00
        assert_eq!(&s[3..7], &[0x44, 0xAC, 0x00, 0x00]);
    }

    #[test]
    fn configure_propagates_einval_from_device() {
        let (client, mock) = setup();
        mock.queue_response(Status::EInval, &[]);
        // The host can ask the device for a wild rate; the device
        // returns EINVAL after clamping/rejecting and the Client
        // forwards it. (The Client itself doesn't pre-clamp because
        // the canonical range is reported by GET_INFO.)
        let err = client.configure(0, 1).unwrap_err();
        assert_eq!(err, crate::errno::EINVAL);
    }

    #[test]
    fn start_then_stop() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        mock.queue_response(Status::Ok, &[]);
        client.start(0).unwrap();
        client.stop(0).unwrap();
        assert_eq!(mock.pop_request().as_slice(), &[0x0A, 0x02, 0]);
        assert_eq!(mock.pop_request().as_slice(), &[0x0A, 0x03, 0]);
    }

    #[test]
    fn write_passes_frames_through() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        // One stereo frame: L=0x0080 (≈ silence + small DC), R=0xFFFF
        let pcm: [u8; 4] = [0x80, 0x00, 0xFF, 0xFF];
        client.write(0, &pcm).unwrap();
        let req = mock.pop_request();
        let s = req.as_slice();
        assert_eq!(&s[..3], &[0x0A, 0x04, 0]);
        assert_eq!(&s[3..7], &pcm);
    }

    #[test]
    fn write_rejects_unaligned_or_empty() {
        let (client, _mock) = setup();
        assert_eq!(client.write(0, &[]).unwrap_err(), crate::errno::EINVAL);
        assert_eq!(client.write(0, &[1, 2, 3]).unwrap_err(),
                   crate::errno::EINVAL);
    }

    #[test]
    fn write_rejects_oversize() {
        let (client, _mock) = setup();
        let big = [0u8; 5000];
        let err = client.write(0, &big).unwrap_err();
        assert_eq!(err, crate::errno::EINVAL);
    }
}
