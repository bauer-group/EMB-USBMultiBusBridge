// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-iio — RPBP ADC subsystem client + auxiliary iio_dev child.
// Wire format in docs/protocol/subsystems/adc.md.

use kernel::prelude::*;
use kernel::sync::Arc;

use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;
use crate::rpbp::{CHAN_CONTROL, Status};
use crate::transport::RoundTrip;

/* ═════ Layer 1 — wire ════════════════════════════════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::adc as ad};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Adc as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 READ — [channel] → [raw LE16][mv LE16] */
    pub fn build_read(dst: &mut [u8], channel: u8) -> Result<usize, i32> {
        let n = start(dst, ad::READ)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = channel;
        Ok(n + 1)
    }

    /// (raw, millivolts) from a READ response.
    pub fn parse_read(resp: &[u8]) -> Result<(u16, u16), i32> {
        if resp.len() < 4 { return Err(crate::errno::EPROTO); }
        Ok((rpbp::rd_le16(&resp[0..2]), rpbp::rd_le16(&resp[2..4])))
    }

    /* 0x01 READ_MANY — [channel_mask LE16] */
    pub fn build_read_many(dst: &mut [u8], channel_mask: u16) -> Result<usize, i32> {
        let n = start(dst, ad::READ_MANY)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        rpbp::wr_le16(&mut dst[n..], channel_mask);
        Ok(n + 2)
    }

    pub struct ManySample { pub channel: u8, pub raw: u16, pub mv: u16 }

    pub fn parse_read_many(
        resp: &[u8], out: &mut Vec<ManySample>,
    ) -> Result<(), i32> {
        if resp.is_empty() { return Err(crate::errno::EPROTO); }
        let count = resp[0] as usize;
        let need = 1 + count * 5;
        if resp.len() < need { return Err(crate::errno::EPROTO); }
        out.clear();
        out.reserve(count, GFP_KERNEL)?;
        for i in 0..count {
            let o = 1 + i * 5;
            out.push(ManySample {
                channel: resp[o],
                raw:     rpbp::rd_le16(&resp[o + 1..o + 3]),
                mv:      rpbp::rd_le16(&resp[o + 3..o + 5]),
            }, GFP_KERNEL)?;
        }
        Ok(())
    }

    /* 0x02 GET_REF — → [vref_mv LE32] */
    pub fn build_get_ref(dst: &mut [u8]) -> Result<usize, i32> {
        start(dst, ad::GET_REF)
    }

    pub fn parse_vref_mv(resp: &[u8]) -> Result<u32, i32> {
        if resp.len() < 4 { return Err(crate::errno::EPROTO); }
        Ok(rpbp::rd_le32(&resp[..4]))
    }

    /* 0x03 TEMP_READ — → [cdegC LE16 i16] */
    pub fn build_temp_read(dst: &mut [u8]) -> Result<usize, i32> {
        start(dst, ad::TEMP_READ)
    }

    pub fn parse_temp_cdeg(resp: &[u8]) -> Result<i16, i32> {
        if resp.len() < 2 { return Err(crate::errno::EPROTO); }
        Ok(i16::from_le_bytes([resp[0], resp[1]]))
    }

    /* 0x04 STREAM_START — [channel LE16][chan_mask][rsv][rate LE32] */
    pub fn build_stream_start(
        dst: &mut [u8], rpbp_channel: u16, channel_mask: u8, rate_hz: u32,
    ) -> Result<usize, i32> {
        let n = start(dst, ad::STREAM_START)?;
        if dst.len() < n + 8 { return Err(crate::errno::EINVAL); }
        rpbp::wr_le16(&mut dst[n..], rpbp_channel);
        dst[n + 2] = channel_mask;
        dst[n + 3] = 0;
        rpbp::wr_le32(&mut dst[n + 4..], rate_hz);
        Ok(n + 8)
    }

    pub fn parse_stream_start(resp: &[u8]) -> Result<u16, i32> {
        if resp.len() < 2 { return Err(crate::errno::EPROTO); }
        Ok(rpbp::rd_le16(&resp[..2]))
    }

    /* 0x05 STREAM_STOP — [channel LE16] */
    pub fn build_stream_stop(dst: &mut [u8], rpbp_channel: u16) -> Result<usize, i32> {
        let n = start(dst, ad::STREAM_STOP)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        rpbp::wr_le16(&mut dst[n..], rpbp_channel);
        Ok(n + 2)
    }

    /// STREAM_DATA payload layout (per docs/protocol/subsystems/adc.md
    /// §STREAM_DATA). One tick carries a µs timestamp + N samples in
    /// ascending channel order.
    pub struct StreamTick {
        pub ts_us:        u32,
        pub channel_mask: u8,
        pub samples:      KVec<u16>,
    }

    pub fn parse_stream_data(payload: &[u8]) -> Result<StreamTick, i32> {
        if payload.len() < 6 { return Err(crate::errno::EPROTO); }
        let ts_us        = rpbp::rd_le32(&payload[0..4]);
        let channel_mask = payload[4];
        let sample_count = payload[5] as usize;
        let need = 6 + sample_count * 2;
        if payload.len() < need { return Err(crate::errno::EPROTO); }
        let mut samples = KVec::new();
        samples.reserve(sample_count, GFP_KERNEL)?;
        for i in 0..sample_count {
            let o = 6 + i * 2;
            samples.push(rpbp::rd_le16(&payload[o..o + 2]), GFP_KERNEL)
                .map_err(|_| crate::errno::ENOMEM)?;
        }
        Ok(StreamTick { ts_us, channel_mask, samples })
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

    /// Single-shot voltage read on one channel.
    /// Returns (raw_code, millivolts).
    pub fn read(&self, channel: u8) -> Result<(u16, u16), i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_read(&mut buf, channel)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_read(resp.as_slice())
    }

    /// Batched read — fills samples in ascending channel order.
    pub fn read_many(&self, channel_mask: u16) -> Result<Vec<wire::ManySample>, i32> {
        let mut buf = [0u8; 4];
        let n = wire::build_read_many(&mut buf, channel_mask)?;
        let resp = self.rt_call(&buf[..n])?;
        let mut out = Vec::new();
        wire::parse_read_many(resp.as_slice(), &mut out)?;
        Ok(out)
    }

    pub fn vref_mv(&self) -> Result<u32, i32> {
        let mut buf = [0u8; 2];
        let n = wire::build_get_ref(&mut buf)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_vref_mv(resp.as_slice())
    }

    /// Read the on-die temperature sensor in centi-degrees Celsius.
    /// `TEMP_READ` already applies the datasheet calibration in
    /// firmware, so the returned value is ready to expose as
    /// `IIO_CHAN_INFO_PROCESSED / 100`.
    pub fn temp_cdeg(&self) -> Result<i16, i32> {
        let mut buf = [0u8; 2];
        let n = wire::build_temp_read(&mut buf)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_temp_cdeg(resp.as_slice())
    }

    /// Open a continuous sampling stream. `rpbp_channel` must be in
    /// the dynamic range 16..239 — the caller allocates it (there is
    /// no CMD_REQUEST{OPEN_CHANNEL} opcode in v1; the driver picks
    /// an unused channel, issues STREAM_START, and starts granting
    /// credit on it).
    ///
    /// Returns the actual channel the device accepted (echoed).
    /// Host is responsible for sending STREAM_CREDIT frames — one
    /// convenience helper lives on RpbridgeState::submit_oneway.
    pub fn stream_start(
        &self, rpbp_channel: u16, channel_mask: u8, rate_hz: u32,
    ) -> Result<u16, i32> {
        let mut buf = [0u8; 10];
        let n = wire::build_stream_start(&mut buf, rpbp_channel, channel_mask, rate_hz)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_stream_start(resp.as_slice())
    }

    pub fn stream_stop(&self, rpbp_channel: u16) -> Result<(), i32> {
        let mut buf = [0u8; 4];
        let n = wire::build_stream_stop(&mut buf, rpbp_channel)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }
}

/* ═════ Layer 3 — auxiliary_device adapter ════════════════════════ */

/// Channel descriptor emitted into the capability-map walk. IIO
/// channels end up as `iio_chan_spec` entries: external inputs are
/// `IIO_VOLTAGE`, the on-die sensor is `IIO_TEMPERATURE`.
#[derive(Copy, Clone, Debug)]
pub enum ChanKind { Voltage, Temperature }

pub struct ChanDesc {
    pub fw_channel: u8,
    pub gp_pin:     Option<u8>,
    pub kind:       ChanKind,
}

#[allow(dead_code)]
pub(crate) struct IioChild {
    client: Client<RpbridgeState>,
    #[allow(dead_code)]
    vref_mv: u32,
    #[allow(dead_code)]
    idx:     u16,
}

impl IioChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state);
        // Best-effort: fetch vref the firmware is actually using. On
        // round-trip failure (probe ordering), fall back to the
        // capability map's max_freq_hz field, which our cap parser
        // has already populated with vref_mv for the ADC entry.
        let vref_mv = client.vref_mv().unwrap_or(bus.max_freq_hz.max(3300));
        pr_info!("rpbridge-iio.{}: probe (vref={}mV)\n", bus.idx, vref_mv);

        // TODO §R4L-IIO-BINDINGS:
        //
        //   Once kernel::iio::Device is wired up:
        //
        //     channels[0..4] = iio_chan_spec {
        //         type = IIO_VOLTAGE,
        //         channel = ch,
        //         indexed = true,
        //         info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
        //         info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
        //         scan_type = { .sign='u', .realbits=12, .storagebits=16, .endianness=IIO_LE },
        //     }
        //     channels[4] = iio_chan_spec {
        //         type = IIO_TEMPERATURE,
        //         info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
        //     }
        //
        //     read_raw(ch, info) match info {
        //         RAW       => client.read(ch.channel).raw,
        //         SCALE     => vref_mv / 4096 (Q0.0 scaled),
        //         PROCESSED => client.temp_cdeg() / 100 (for temperature ch),
        //     }
        //
        //   Triggered-buffer path: on buffer_postenable(),
        //   client.stream_start(chosen_channel, active_mask, rate_hz).
        //   In the event-pump (core_driver handle_stream_data), call
        //   iio_push_to_buffers_with_timestamp() once per StreamTick.
        //   buffer_predisable() → client.stream_stop().

        Ok(IioChild { client, vref_mv, idx: bus.idx })
    }

    #[allow(dead_code)]
    pub(crate) fn client(&self) -> &Client<RpbridgeState> { &self.client }
}

/* ═════ Tests ═════════════════════════════════════════════════════ */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpbp::op::adc as ad;

    #[test]
    fn build_read_layout() {
        let mut buf = [0u8; 3];
        let n = wire::build_read(&mut buf, 2).unwrap();
        assert_eq!(n, 3);
        assert_eq!(buf, [0x05, ad::READ, 2]);
    }

    #[test]
    fn parse_read_extracts_raw_and_mv() {
        let resp = [0x34, 0x12, 0xAB, 0x05];
        let (raw, mv) = wire::parse_read(&resp).unwrap();
        assert_eq!(raw, 0x1234);
        assert_eq!(mv, 0x05AB);
    }

    #[test]
    fn parse_read_many_honours_count() {
        let resp = [
            3,                           // count = 3
            0, 0x10, 0x00, 0xE8, 0x03,   // ch=0 raw=0x0010 mv=1000
            1, 0x20, 0x00, 0xE8, 0x03,
            2, 0x30, 0x00, 0xE8, 0x03,
        ];
        let mut out = Vec::new();
        wire::parse_read_many(&resp, &mut out).unwrap();
        assert_eq!(out.len(), 3);
        assert_eq!(out[0].channel, 0);
        assert_eq!(out[0].raw, 0x0010);
        assert_eq!(out[2].channel, 2);
    }

    #[test]
    fn temp_cdeg_signed() {
        // -500 cdegC = 0xFE0C LE.
        let resp: [u8; 2] = (-500i16).to_le_bytes();
        assert_eq!(wire::parse_temp_cdeg(&resp).unwrap(), -500);
    }

    #[test]
    fn stream_start_layout() {
        let mut buf = [0u8; 10];
        let n = wire::build_stream_start(&mut buf, 16, 0b00001111, 1_000).unwrap();
        assert_eq!(n, 10);
        assert_eq!(buf, [
            0x05, ad::STREAM_START,
            16, 0,          // channel LE16 = 16
            0b1111, 0,      // mask, rsv
            0xE8, 0x03, 0, 0, // rate LE32 = 1000
        ]);
    }

    #[test]
    fn stream_data_parse_multi_sample() {
        // ts=0x1111_2222, mask=0b1111, count=4, samples 100 200 300 400
        let payload = [
            0x22, 0x22, 0x11, 0x11,
            0b1111, 4,
            100, 0, 200, 0, 0x2C, 0x01, 0x90, 0x01,
        ];
        let tick = wire::parse_stream_data(&payload).unwrap();
        assert_eq!(tick.ts_us, 0x1111_2222);
        assert_eq!(tick.channel_mask, 0b1111);
        assert_eq!(tick.samples.as_slice(), &[100, 200, 300, 400]);
    }
}
