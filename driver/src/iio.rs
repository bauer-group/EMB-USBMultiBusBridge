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

/* ═════ Layer 3 — iio_device kernel integration ═════════════════ */

use crate::ffi;

/// Channel descriptor emitted into the capability-map walk. IIO
/// channels end up as `iio_chan_spec` entries: external inputs are
/// `IIO_VOLTAGE`, the on-die sensor is `IIO_TEMPERATURE`.
#[derive(Copy, Clone, Debug)]
pub enum ChanKind { Voltage, Temperature }

/// On rev-B: channels 0..3 are external voltage inputs (GP26..29),
/// channel 4 is the on-die temperature sensor.
const VOLTAGE_CHANNEL_COUNT: i32 = 4;
const TEMP_CHANNEL:          i32 = 4;
const TOTAL_CHANNELS:        u32 = 5;

struct IioBackendImpl {
    client:  Client<RpbridgeState>,
    vref_mv: u32,
}

impl ffi::iio::IioBackend for IioBackendImpl {
    fn read(&self, channel: i32, info: ffi::iio::IioInfo)
        -> Result<ffi::iio::IioReadVal, i32>
    {
        use ffi::iio::{IioInfo, IioReadVal};
        use crate::ffi::bindings as cc;

        // Temperature channel uses a processed read regardless of
        // whether the kernel asked for RAW or PROCESSED — the
        // firmware already applies datasheet calibration.
        if channel == TEMP_CHANNEL {
            let cdeg = self.client.temp_cdeg()?;
            return Ok(IioReadVal {
                val:  cdeg as i32 * 10,     // cdeg → millidegrees C
                val2: 0,
                ret:  cc::IIO_VAL_INT,
            });
        }

        if channel < 0 || channel >= VOLTAGE_CHANNEL_COUNT {
            return Err(crate::errno::EINVAL);
        }

        match info {
            IioInfo::Raw => {
                let (raw, _mv) = self.client.read(channel as u8)?;
                Ok(IioReadVal {
                    val:  raw as i32,
                    val2: 0,
                    ret:  cc::IIO_VAL_INT,
                })
            }
            IioInfo::Processed => {
                let (_raw, mv) = self.client.read(channel as u8)?;
                Ok(IioReadVal {
                    val:  mv as i32,
                    val2: 0,
                    ret:  cc::IIO_VAL_INT,
                })
            }
            IioInfo::Scale => {
                // 12-bit ADC, vref_mv reference. Scale expresses
                // "volts per count" = vref_mv / 4096 mV/count.
                // IIO expects IIO_VAL_FRACTIONAL: val / val2.
                Ok(IioReadVal {
                    val:  self.vref_mv as i32,
                    val2: 4096,
                    ret:  cc::IIO_VAL_FRACTIONAL,
                })
            }
        }
    }
}

pub(crate) struct IioChild {
    _handle: ffi::iio::DeviceHandle,
    #[allow(dead_code)]
    vref_mv: u32,
    #[allow(dead_code)]
    idx:     u16,
}

impl IioChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state.clone());
        let vref_mv = client.vref_mv().unwrap_or(3300);
        pr_info!("rpbridge-iio.{}: probe (vref={}mV, {} channels)\n",
                 bus.idx, vref_mv, TOTAL_CHANNELS);

        let parent = crate::ffi::auxiliary::parent_device(
            &*core::ops::Deref::deref(&state)
        ).map_err(|e| kernel::error::Error::from_errno(e))?;

        let backend = IioBackendImpl { client, vref_mv };
        let handle = ffi::iio::DeviceHandle::register(parent, TOTAL_CHANNELS, backend)
            .map_err(|e| kernel::error::Error::from_errno(e))?;
        Ok(IioChild { _handle: handle, vref_mv, idx: bus.idx })
    }
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

/* ═════ Client-level integration tests ═══════════════════════════ */

#[cfg(test)]
mod client_tests {
    use super::*;
    use crate::transport::MockRoundTrip;
    use crate::rpbp::Status;

    fn setup() -> (Client<MockRoundTrip>, Arc<MockRoundTrip>) {
        let mock = Arc::new(MockRoundTrip::new());
        let client = Client::new(mock.clone());
        (client, mock)
    }

    #[test]
    fn read_returns_raw_and_mv() {
        let (client, mock) = setup();
        // raw=0x0800 (2048), mv=1650
        mock.queue_response(Status::Ok, &[0x00, 0x08, 0x72, 0x06]);
        let (raw, mv) = client.read(2).unwrap();
        assert_eq!(raw, 0x0800);
        assert_eq!(mv, 1650);

        let req = mock.pop_request();
        // subsys=ADC=5, op=READ=0, channel=2
        assert_eq!(req.as_slice(), &[0x05, 0x00, 2]);
    }

    #[test]
    fn read_many_returns_samples_in_channel_order() {
        let (client, mock) = setup();
        // count=3, then 3 ManySamples of 5 bytes each
        let mut resp = KVec::new();
        resp.push(3, GFP_KERNEL).unwrap();
        for (ch, raw, mv) in [(0u8, 100u16, 250u16),
                              (1, 200, 500),
                              (2, 300, 750)] {
            resp.push(ch, GFP_KERNEL).unwrap();
            resp.extend_from_slice(&raw.to_le_bytes(), GFP_KERNEL).unwrap();
            resp.extend_from_slice(&mv.to_le_bytes(), GFP_KERNEL).unwrap();
        }
        mock.queue_response(Status::Ok, resp.as_slice());

        let samples = client.read_many(0b0111).unwrap();
        assert_eq!(samples.len(), 3);
        assert_eq!(samples[0].channel, 0);
        assert_eq!(samples[0].raw, 100);
        assert_eq!(samples[2].mv, 750);
    }

    #[test]
    fn vref_mv_returns_le32() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &3300u32.to_le_bytes());
        assert_eq!(client.vref_mv().unwrap(), 3300);
    }

    #[test]
    fn temp_cdeg_returns_signed_cdeg() {
        let (client, mock) = setup();
        // 2500 cdegC = 25.00 °C
        mock.queue_response(Status::Ok, &2500i16.to_le_bytes());
        assert_eq!(client.temp_cdeg().unwrap(), 2500);
        // Negative temperatures also round-trip.
        mock.queue_response(Status::Ok, &(-500i16).to_le_bytes());
        assert_eq!(client.temp_cdeg().unwrap(), -500);
    }

    #[test]
    fn stream_start_and_stop_roundtrip() {
        let (client, mock) = setup();
        // Start reply: actual_channel=16 LE16
        mock.queue_response(Status::Ok, &[0x10, 0x00]);
        mock.queue_response(Status::Ok, &[]);

        let ch = client.stream_start(16, 0b0001, 1000).unwrap();
        assert_eq!(ch, 16);
        client.stream_stop(16).unwrap();

        let start_req = mock.pop_request();
        let stop_req  = mock.pop_request();
        // start: subsys=5, op=4, channel LE16, mask, rsv=0, rate LE32
        assert_eq!(start_req.as_slice()[..2], [0x05, 0x04]);
        assert_eq!(&start_req.as_slice()[2..4], &[0x10, 0x00]);
        assert_eq!(start_req.as_slice()[4], 0b0001);
        // stop: subsys=5, op=5, channel LE16
        assert_eq!(stop_req.as_slice(), &[0x05, 0x05, 0x10, 0x00]);
    }

    #[test]
    fn stream_start_ebusy_on_second_open() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[0x10, 0x00]);
        mock.queue_response(Status::EBusy, &[]);

        client.stream_start(16, 0b0001, 1000).unwrap();
        let err = client.stream_start(16, 0b0001, 1000).unwrap_err();
        assert_eq!(err, 16); // EBUSY
    }

    #[test]
    fn read_propagates_enodev_on_invalid_channel() {
        let (client, mock) = setup();
        mock.queue_response(Status::ENoDev, &[]);
        let err = client.read(99).unwrap_err();
        assert_eq!(err, 19); // ENODEV
    }
}
