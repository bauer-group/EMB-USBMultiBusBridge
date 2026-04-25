// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-onewire — RPBP 1-Wire (Maxim/Dallas) bus master client.
// Wire format in docs/protocol/subsystems/onewire.md.
//
// Layer 1 (`wire`) builds the request bytes and parses replies.
// Layer 2 (`Client<T>`) glues those builders to the round-trip
// transport. Layer 3 (kernel binding via `kernel::w1::Master`) is
// deferred until upstream R4L lands a w1_master wrapper.
//
// The byte-level helpers (read_byte / write_byte / search_rom) are
// intentionally NOT done in firmware — bit-slot operations are the
// minimum quantum the PIO program understands, and folding bytes in
// the host gives the kernel w1 stack the bit granularity it expects.

use kernel::prelude::*;
use kernel::sync::Arc;

use crate::rpbp::{CHAN_CONTROL, Status};
use crate::transport::RoundTrip;

/* ═════ Layer 1 — wire ════════════════════════════════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::onewire as ow};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Onewire as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 GET_INFO — empty args */
    pub fn build_get_info(dst: &mut [u8]) -> Result<usize, i32> {
        start(dst, ow::GET_INFO)
    }

    pub struct OnewireInfo {
        pub count: u8,
        pub pin:   u8,
        pub speed: u8,
    }

    pub fn parse_info(resp: &[u8]) -> Result<OnewireInfo, i32> {
        if resp.len() < 4 { return Err(crate::errno::EPROTO); }
        Ok(OnewireInfo {
            count: resp[0],
            pin:   resp[1],
            speed: resp[2],
        })
    }

    /* 0x01 RESET — [idx u8] */
    pub fn build_reset(dst: &mut [u8], idx: u8) -> Result<usize, i32> {
        let n = start(dst, ow::RESET)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        Ok(n + 1)
    }

    pub fn parse_presence(resp: &[u8]) -> Result<bool, i32> {
        if resp.is_empty() { return Err(crate::errno::EPROTO); }
        Ok(resp[0] != 0)
    }

    /* 0x02 WRITE — [idx u8][bytes …] */
    pub fn build_write(
        dst: &mut [u8], idx: u8, bytes: &[u8],
    ) -> Result<usize, i32> {
        let n = start(dst, ow::WRITE)?;
        if dst.len() < n + 1 + bytes.len() { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        dst[n + 1..n + 1 + bytes.len()].copy_from_slice(bytes);
        Ok(n + 1 + bytes.len())
    }

    /* 0x03 READ — [idx u8][len LE u16] */
    pub fn build_read(
        dst: &mut [u8], idx: u8, len: u16,
    ) -> Result<usize, i32> {
        let n = start(dst, ow::READ)?;
        if dst.len() < n + 3 { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        rpbp::wr_le16(&mut dst[n + 1..], len);
        Ok(n + 3)
    }

    /* 0x04 TRIPLET — [idx u8][dir u8] */
    pub fn build_triplet(
        dst: &mut [u8], idx: u8, dir: u8,
    ) -> Result<usize, i32> {
        let n = start(dst, ow::TRIPLET)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        dst[n + 1] = dir & 1;
        Ok(n + 2)
    }

    /// Decoded TRIPLET result: id_bit / cmp_bit / direction-written.
    /// Mirrors the firmware-side packing so the host can drive the
    /// SEARCH ROM state machine directly (Maxim AN187).
    pub struct TripletResult {
        pub id_bit:  bool,
        pub cmp_bit: bool,
        pub dir:     bool,
    }

    pub fn parse_triplet(resp: &[u8]) -> Result<TripletResult, i32> {
        if resp.is_empty() { return Err(crate::errno::EPROTO); }
        let v = resp[0];
        Ok(TripletResult {
            id_bit:  (v & 0x01) != 0,
            cmp_bit: (v & 0x02) != 0,
            dir:     (v & 0x04) != 0,
        })
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

    pub fn info(&self) -> Result<wire::OnewireInfo, i32> {
        let mut buf = [0u8; 2];
        let n = wire::build_get_info(&mut buf)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_info(resp.as_slice())
    }

    /// Issue a reset pulse and return whether at least one slave
    /// asserted a presence pulse. Maps directly to the Linux w1
    /// `reset_bus()` callback.
    pub fn reset(&self, idx: u8) -> Result<bool, i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_reset(&mut buf, idx)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_presence(resp.as_slice())
    }

    pub fn write(&self, idx: u8, bytes: &[u8]) -> Result<(), i32> {
        if bytes.len() > crate::rpbp::MAX_PAYLOAD_LEN.saturating_sub(8) {
            return Err(crate::errno::EINVAL);
        }
        let mut buf = KVec::new();
        buf.resize(3 + bytes.len(), 0u8, GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        let n = wire::build_write(buf.as_mut_slice(), idx, bytes)?;
        let _ = self.rt_call(&buf.as_slice()[..n])?;
        Ok(())
    }

    pub fn read(&self, idx: u8, len: u16) -> Result<KVec<u8>, i32> {
        if len == 0 { return Err(crate::errno::EINVAL); }
        let mut buf = [0u8; 5];
        let n = wire::build_read(&mut buf, idx, len)?;
        self.rt_call(&buf[..n])
    }

    /// SEARCH ROM triplet primitive. The kernel w1 SEARCH state
    /// machine calls this once per ROM bit (64 times per slave) so
    /// it's hot — the single round-trip-per-call overhead is the
    /// reason we expose it as a first-class opcode rather than
    /// composing two read_bit + one write_bit on the host side.
    pub fn triplet(
        &self, idx: u8, dir: u8,
    ) -> Result<wire::TripletResult, i32> {
        let mut buf = [0u8; 4];
        let n = wire::build_triplet(&mut buf, idx, dir)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_triplet(resp.as_slice())
    }
}

/* ═════ Layer 3 — child probe (placeholder) ═══════════════════════ */

use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;

pub(crate) struct OnewireChild {
    #[allow(dead_code)]
    client: Client<RpbridgeState>,
    #[allow(dead_code)]
    idx:    u16,
}

impl OnewireChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state.clone());
        match client.info() {
            Ok(info) => pr_info!(
                "rpbridge-onewire.{}: pin={} speed={} (no kernel binding yet)\n",
                bus.idx, info.pin, info.speed),
            Err(e) => pr_warn!(
                "rpbridge-onewire.{}: GET_INFO failed: {}\n", bus.idx, e),
        }
        Ok(OnewireChild { client, idx: bus.idx })
    }
}

/* ═════ Tests ═════════════════════════════════════════════════════ */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpbp::op::onewire as ow;

    #[test]
    fn build_get_info_layout() {
        let mut buf = [0u8; 2];
        let n = wire::build_get_info(&mut buf).unwrap();
        assert_eq!(n, 2);
        assert_eq!(buf, [0x09, ow::GET_INFO]);
    }

    #[test]
    fn parse_info_extracts_fields() {
        let resp: [u8; 4] = [1, 32, 0, 0];
        let info = wire::parse_info(&resp).unwrap();
        assert_eq!(info.count, 1);
        assert_eq!(info.pin, 32);
        assert_eq!(info.speed, 0);
    }

    #[test]
    fn build_reset_layout() {
        let mut buf = [0u8; 3];
        let n = wire::build_reset(&mut buf, 0).unwrap();
        assert_eq!(n, 3);
        assert_eq!(buf, [0x09, ow::RESET, 0]);
    }

    #[test]
    fn parse_presence_round_trip() {
        assert_eq!(wire::parse_presence(&[0x00]).unwrap(), false);
        assert_eq!(wire::parse_presence(&[0x01]).unwrap(), true);
        assert_eq!(wire::parse_presence(&[0xFF]).unwrap(), true);
    }

    #[test]
    fn build_write_carries_payload() {
        let mut buf = [0u8; 16];
        let body: [u8; 5] = [0x33, 0xCC, 0x44, 0xAA, 0x55];
        let n = wire::build_write(&mut buf, 0, &body).unwrap();
        assert_eq!(n, 8);
        assert_eq!(&buf[..3], &[0x09, ow::WRITE, 0]);
        assert_eq!(&buf[3..8], &body);
    }

    #[test]
    fn build_read_layout() {
        let mut buf = [0u8; 5];
        let n = wire::build_read(&mut buf, 0, 0x0009).unwrap();
        assert_eq!(n, 5);
        assert_eq!(buf, [0x09, ow::READ, 0, 0x09, 0x00]);
    }

    #[test]
    fn parse_triplet_decodes_all_combinations() {
        // id=0, cmp=0, dir=0  → 0b000
        let t = wire::parse_triplet(&[0b000]).unwrap();
        assert!(!t.id_bit && !t.cmp_bit && !t.dir);
        // id=1, cmp=1, dir=0  → 0b011 (slave broke, both bits 1 — error)
        let t = wire::parse_triplet(&[0b011]).unwrap();
        assert!(t.id_bit && t.cmp_bit && !t.dir);
        // id=0, cmp=1, dir=0  → 0b010 (all slaves want bit 0)
        let t = wire::parse_triplet(&[0b010]).unwrap();
        assert!(!t.id_bit && t.cmp_bit && !t.dir);
        // id=1, cmp=0, dir=1  → 0b101 (all slaves want bit 1, host wrote 1)
        let t = wire::parse_triplet(&[0b101]).unwrap();
        assert!(t.id_bit && !t.cmp_bit && t.dir);
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
    fn reset_returns_presence_flag() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[1u8]);
        assert_eq!(client.reset(0).unwrap(), true);
    }

    #[test]
    fn reset_returns_no_presence() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[0u8]);
        assert_eq!(client.reset(0).unwrap(), false);
    }

    #[test]
    fn write_emits_correct_bytes() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        let body: [u8; 4] = [0xCC, 0x44, 0xBE, 0x00];
        client.write(0, &body).unwrap();
        let req = mock.pop_request();
        let s = req.as_slice();
        assert_eq!(s[..3], [0x09, 0x02, 0]);
        assert_eq!(&s[3..7], &body);
    }

    #[test]
    fn read_returns_payload() {
        let (client, mock) = setup();
        let body: [u8; 9] = [0x28, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE];
        mock.queue_response(Status::Ok, &body);
        let v = client.read(0, 9).unwrap();
        assert_eq!(v.as_slice(), &body);
    }

    #[test]
    fn read_zero_length_rejected() {
        let (client, _mock) = setup();
        let err = client.read(0, 0).unwrap_err();
        assert_eq!(err, crate::errno::EINVAL);
    }

    #[test]
    fn triplet_decodes_response() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[0b101u8]);
        let t = client.triplet(0, 1).unwrap();
        assert!(t.id_bit);
        assert!(!t.cmp_bit);
        assert!(t.dir);
    }

    #[test]
    fn write_oversize_rejected() {
        let (client, _mock) = setup();
        let big = [0u8; 5000];
        let err = client.write(0, &big).unwrap_err();
        assert_eq!(err, crate::errno::EINVAL);
    }
}
