// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-dmx — RPBP DMX subsystem client. Wire format in
// docs/protocol/subsystems/dmx.md.
//
// DMX has no upstream Linux subsystem (OLA / dmx-net are userspace),
// so this client is the kernel-side abstraction that a future char
// device or netlink interface plugs into. v1 keeps it as a wire-layer
// + Client only; the kernel adapter that exposes `/dev/rpbridge-dmx*`
// is tracked in driver/TODO.md §M12.

use kernel::prelude::*;
use kernel::sync::Arc;

use crate::rpbp::{CHAN_CONTROL, Status};
use crate::transport::RoundTrip;

/* ═════ Layer 1 — wire ════════════════════════════════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::dmx as dx};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Dmx as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 GET_INFO — empty args, returns DmxInfo. */
    pub fn build_get_info(dst: &mut [u8]) -> Result<usize, i32> {
        start(dst, dx::GET_INFO)
    }

    pub struct DmxInfo {
        pub count:      u8,
        pub pin:        u8,
        pub running:    bool,
        pub slot_count: u16,
        pub break_us:   u16,
        pub mab_us:     u16,
    }

    pub fn parse_info(resp: &[u8]) -> Result<DmxInfo, i32> {
        if resp.len() < 10 { return Err(crate::errno::EPROTO); }
        Ok(DmxInfo {
            count:      resp[0],
            pin:        resp[1],
            running:    resp[2] != 0,
            slot_count: rpbp::rd_le16(&resp[4..6]),
            break_us:   rpbp::rd_le16(&resp[6..8]),
            mab_us:     rpbp::rd_le16(&resp[8..10]),
        })
    }

    /* 0x01 SET_SLOTS — [idx u8][offset LE u16][bytes …]
     *
     * Bytes are slot values in DMX channel space. offset = 0 is the
     * start code (slot 0); offset = 1 is channel 1, …, offset = 512
     * is channel 512. */
    pub fn build_set_slots(
        dst: &mut [u8], idx: u8, offset: u16, bytes: &[u8],
    ) -> Result<usize, i32> {
        let n = start(dst, dx::SET_SLOTS)?;
        if dst.len() < n + 3 + bytes.len() { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        rpbp::wr_le16(&mut dst[n + 1..], offset);
        dst[n + 3..n + 3 + bytes.len()].copy_from_slice(bytes);
        Ok(n + 3 + bytes.len())
    }

    /* 0x02 / 0x03 START / STOP — [idx u8] */
    pub fn build_start_stop(
        dst: &mut [u8], idx: u8, start_: bool,
    ) -> Result<usize, i32> {
        let op = if start_ { dx::START } else { dx::STOP };
        let n = start(dst, op)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        Ok(n + 1)
    }

    /* 0x04 SET_BREAK — [idx u8][break_us LE u16][mab_us LE u16] */
    pub fn build_set_break(
        dst: &mut [u8], idx: u8, break_us: u16, mab_us: u16,
    ) -> Result<usize, i32> {
        let n = start(dst, dx::SET_BREAK)?;
        if dst.len() < n + 5 { return Err(crate::errno::EINVAL); }
        dst[n] = idx;
        rpbp::wr_le16(&mut dst[n + 1..], break_us);
        rpbp::wr_le16(&mut dst[n + 3..], mab_us);
        Ok(n + 5)
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

    pub fn info(&self) -> Result<wire::DmxInfo, i32> {
        let mut buf = [0u8; 2];
        let n = wire::build_get_info(&mut buf)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_info(resp.as_slice())
    }

    /// Patch the universe at `offset` (slot index) with `bytes`. Does
    /// not start transmission — call `start()` to begin emitting
    /// frames. The firmware persists the universe between writes so
    /// partial updates are cheap.
    pub fn set_slots(
        &self, idx: u8, offset: u16, bytes: &[u8],
    ) -> Result<(), i32> {
        if bytes.len() > crate::rpbp::MAX_PAYLOAD_LEN.saturating_sub(8) {
            return Err(crate::errno::EINVAL);
        }
        let mut buf = KVec::new();
        buf.resize(5 + bytes.len(), 0u8, GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        let n = wire::build_set_slots(buf.as_mut_slice(), idx, offset, bytes)?;
        let _ = self.rt_call(&buf.as_slice()[..n])?;
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

    pub fn set_break(
        &self, idx: u8, break_us: u16, mab_us: u16,
    ) -> Result<(), i32> {
        let mut buf = [0u8; 7];
        let n = wire::build_set_break(&mut buf, idx, break_us, mab_us)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }
}

/* ═════ Layer 3 — child probe (placeholder) ═══════════════════════ *
 *
 * Linux has no in-kernel DMX subsystem; OLA + dmx-net are userspace.
 * Until we ship a char-device wrapper (driver/TODO.md §M12), the
 * child probe just records that the universe exists and lets
 * userspace open it through the vendor RPBP pipe.
 */

use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;

pub(crate) struct DmxChild {
    #[allow(dead_code)]
    client: Client<RpbridgeState>,
    #[allow(dead_code)]
    idx:    u16,
}

impl DmxChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state.clone());
        match client.info() {
            Ok(info) => pr_info!(
                "rpbridge-dmx.{}: pin={} slots={} (no kernel binding yet)\n",
                bus.idx, info.pin, info.slot_count),
            Err(e) => pr_warn!(
                "rpbridge-dmx.{}: GET_INFO failed: {}\n", bus.idx, e),
        }
        Ok(DmxChild { client, idx: bus.idx })
    }
}

/* ═════ Tests ═════════════════════════════════════════════════════ */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpbp::op::dmx as dx;

    #[test]
    fn build_get_info_layout() {
        let mut buf = [0u8; 2];
        let n = wire::build_get_info(&mut buf).unwrap();
        assert_eq!(n, 2);
        assert_eq!(buf, [0x08, dx::GET_INFO]);
    }

    #[test]
    fn parse_info_extracts_all_fields() {
        let resp: [u8; 10] = [
            1,         // count
            44,        // pin
            1,         // running
            0,         // reserved
            0x00, 0x02,    // slot_count = 512
            0x64, 0x00,    // break_us = 100
            0x0C, 0x00,    // mab_us   = 12
        ];
        let info = wire::parse_info(&resp).unwrap();
        assert_eq!(info.count, 1);
        assert_eq!(info.pin, 44);
        assert!(info.running);
        assert_eq!(info.slot_count, 512);
        assert_eq!(info.break_us, 100);
        assert_eq!(info.mab_us, 12);
    }

    #[test]
    fn build_set_slots_carries_payload() {
        let mut buf = [0u8; 16];
        let payload: [u8; 4] = [0x80, 0x40, 0xFF, 0x00];
        let n = wire::build_set_slots(&mut buf, 0, 1, &payload).unwrap();
        assert_eq!(n, 9);
        assert_eq!(&buf[..5], &[0x08, dx::SET_SLOTS, 0, 0x01, 0x00]);
        assert_eq!(&buf[5..9], &payload);
    }

    #[test]
    fn build_start_stop_layout() {
        let mut buf = [0u8; 3];
        let n = wire::build_start_stop(&mut buf, 0, true).unwrap();
        assert_eq!(n, 3);
        assert_eq!(buf, [0x08, dx::START, 0]);

        let n = wire::build_start_stop(&mut buf, 0, false).unwrap();
        assert_eq!(n, 3);
        assert_eq!(buf, [0x08, dx::STOP, 0]);
    }

    #[test]
    fn build_set_break_layout() {
        let mut buf = [0u8; 7];
        let n = wire::build_set_break(&mut buf, 0, 120, 16).unwrap();
        assert_eq!(n, 7);
        assert_eq!(buf, [0x08, dx::SET_BREAK, 0, 0x78, 0x00, 0x10, 0x00]);
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
    fn info_round_trip_parses_running_flag() {
        let (client, mock) = setup();
        let body: [u8; 10] = [1, 44, 0, 0,
                              0x00, 0x02,
                              0x64, 0x00,
                              0x0C, 0x00];
        mock.queue_response(Status::Ok, &body);
        let info = client.info().unwrap();
        assert!(!info.running);
    }

    #[test]
    fn set_slots_emits_correct_bytes() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        let p: [u8; 3] = [0xAA, 0xBB, 0xCC];
        client.set_slots(0, 5, &p).unwrap();
        let req = mock.pop_request();
        let s = req.as_slice();
        assert_eq!(s[..5], [0x08, 0x01, 0, 0x05, 0x00]);
        assert_eq!(&s[5..8], &p);
    }

    #[test]
    fn start_then_stop_two_round_trips() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        mock.queue_response(Status::Ok, &[]);
        client.start(0).unwrap();
        client.stop(0).unwrap();
        assert_eq!(mock.pop_request().as_slice(), &[0x08, 0x02, 0]);
        assert_eq!(mock.pop_request().as_slice(), &[0x08, 0x03, 0]);
    }

    #[test]
    fn set_slots_rejects_oversize_payload() {
        let (client, _mock) = setup();
        let big = [0u8; 5000];
        let err = client.set_slots(0, 0, &big).unwrap_err();
        assert_eq!(err, crate::errno::EINVAL);
    }

    #[test]
    fn set_break_propagates_einval_from_device() {
        let (client, mock) = setup();
        mock.queue_response(Status::EInval, &[]);
        // Spec-violating: break_us < 88. Device returns EINVAL after
        // clamping; our Client forwards it.
        let err = client.set_break(0, 1, 1).unwrap_err();
        assert_eq!(err, crate::errno::EINVAL);
    }
}
