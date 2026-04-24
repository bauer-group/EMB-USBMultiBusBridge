// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-pwm — RPBP PWM subsystem client + auxiliary pwm_chip
// child. Wire format in docs/protocol/subsystems/pwm.md.

use kernel::prelude::*;
use kernel::sync::Arc;

use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;
use crate::rpbp::{CHAN_CONTROL, Status};
use crate::transport::RoundTrip;

/* ═════ Layer 1 — wire ════════════════════════════════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::pwm as pw};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Pwm as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /// Build CONFIGURE — `[pin][flags][duty_q16 LE16][freq_hz LE32]`.
    pub fn build_configure(
        dst: &mut [u8], pin: u8, duty_q16: u16, freq_hz: u32, invert: bool,
    ) -> Result<usize, i32> {
        let n = start(dst, pw::CONFIGURE)?;
        if dst.len() < n + 8 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        dst[n + 1] = if invert { pw::FLAG_INVERT } else { 0 };
        rpbp::wr_le16(&mut dst[n + 2..], duty_q16);
        rpbp::wr_le32(&mut dst[n + 4..], freq_hz);
        Ok(n + 8)
    }

    pub fn build_set_duty(dst: &mut [u8], pin: u8, duty_q16: u16) -> Result<usize, i32> {
        let n = start(dst, pw::SET_DUTY)?;
        if dst.len() < n + 3 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        rpbp::wr_le16(&mut dst[n + 1..], duty_q16);
        Ok(n + 3)
    }

    pub fn build_start_stop(dst: &mut [u8], pin: u8, start_: bool) -> Result<usize, i32> {
        let op = if start_ { pw::START } else { pw::STOP };
        let n = start(dst, op)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        Ok(n + 1)
    }

    pub fn build_get_state(dst: &mut [u8], pin: u8) -> Result<usize, i32> {
        let n = start(dst, pw::GET_STATE)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = pin;
        Ok(n + 1)
    }

    pub struct PwmState {
        pub freq_hz:  u32,
        pub duty_q16: u16,
        pub running:  bool,
        pub invert:   bool,
        pub slice:    u8,
        pub channel:  u8,
    }

    pub fn parse_state(resp: &[u8]) -> Result<PwmState, i32> {
        if resp.len() < 10 { return Err(crate::errno::EPROTO); }
        Ok(PwmState {
            freq_hz:  rpbp::rd_le32(&resp[0..4]),
            duty_q16: rpbp::rd_le16(&resp[4..6]),
            running:  resp[6] != 0,
            invert:   resp[7] != 0,
            slice:    resp[8],
            channel:  resp[9],
        })
    }
}

/* ═════ Layer 2 — Client ══════════════════════════════════════════ */

/// Canonical pwm_state → wire conversion. The Linux pwm_state
/// expresses period and duty in nanoseconds with polarity; the
/// firmware wants frequency-Hz + duty-as-Q0.16 + invert flag.
///
/// `period_ns`: 10..4_000_000_000 (250 MHz .. 0.25 Hz). `duty_ns` is
/// clamped to `period_ns`. Returns (freq_hz, duty_q16).
pub fn encode_state(period_ns: u64, duty_ns: u64, polarity_invert: bool)
    -> Result<(u32, u16, bool), i32>
{
    if period_ns < 10 || period_ns > 4_000_000_000 {
        return Err(crate::errno::EINVAL);
    }
    let duty = duty_ns.min(period_ns);
    // freq_hz = 1e9 / period_ns (integer math; round down).
    let freq_hz = (1_000_000_000u64 / period_ns) as u32;
    // duty_q16 = 65535 * duty / period, saturating at 65535.
    let duty_q16 = ((duty.saturating_mul(65535)) / period_ns)
        .min(65535) as u16;
    Ok((freq_hz, duty_q16, polarity_invert))
}

/// Reverse direction: when the kernel asks for the current pwm_state
/// we need to turn PwmState back into (period_ns, duty_ns).
pub fn decode_state(st: &wire::PwmState) -> (u64, u64) {
    let period_ns = if st.freq_hz == 0 { 0 } else {
        1_000_000_000u64 / u64::from(st.freq_hz)
    };
    let duty_ns = (period_ns.saturating_mul(u64::from(st.duty_q16))) / 65535;
    (period_ns, duty_ns)
}

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

    /// Apply a canonical pwm_state to a pin. Does CONFIGURE then
    /// START/STOP based on `enabled`. Kernel adapter passes through
    /// pwm_apply_state() directly.
    pub fn apply_state(
        &self, pin: u8, period_ns: u64, duty_ns: u64,
        polarity_invert: bool, enabled: bool,
    ) -> Result<(), i32> {
        let (freq_hz, duty_q16, invert) =
            encode_state(period_ns, duty_ns, polarity_invert)?;
        let mut buf = [0u8; 10];
        let n = wire::build_configure(&mut buf, pin, duty_q16, freq_hz, invert)?;
        let _ = self.rt_call(&buf[..n])?;
        let mut sbuf = [0u8; 3];
        let sn = wire::build_start_stop(&mut sbuf, pin, enabled)?;
        let _ = self.rt_call(&sbuf[..sn])?;
        Ok(())
    }

    pub fn set_duty_q16(&self, pin: u8, duty_q16: u16) -> Result<(), i32> {
        let mut buf = [0u8; 5];
        let n = wire::build_set_duty(&mut buf, pin, duty_q16)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn start(&self, pin: u8) -> Result<(), i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_start_stop(&mut buf, pin, true)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn stop(&self, pin: u8) -> Result<(), i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_start_stop(&mut buf, pin, false)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn get_state(&self, pin: u8) -> Result<wire::PwmState, i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_get_state(&mut buf, pin)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_state(resp.as_slice())
    }
}

/* ═════ Layer 3 — pwm_chip kernel integration ═══════════════════ */

use crate::ffi;

struct PwmBackendImpl {
    client: Client<RpbridgeState>,
    pins:   Vec<u8>,
}

impl ffi::pwm::PwmBackend for PwmBackendImpl {
    fn apply(&self, hwpwm: u32, state: &ffi::pwm::PwmState) -> i32 {
        let Some(&pin) = self.pins.as_slice().get(hwpwm as usize) else {
            return -crate::errno::ENODEV;
        };
        let polarity_invert = matches!(state.polarity, ffi::pwm::Polarity::Inversed);
        match self.client.apply_state(
            pin, state.period_ns, state.duty_ns,
            polarity_invert, state.enabled,
        ) {
            Ok(())  => 0,
            Err(e)  => -e,
        }
    }

    fn get_state(&self, hwpwm: u32) -> Result<ffi::pwm::PwmState, i32> {
        let pin = *self.pins.as_slice().get(hwpwm as usize)
            .ok_or(crate::errno::ENODEV)?;
        let s = self.client.get_state(pin)?;
        let (period_ns, duty_ns) = decode_state(&s);
        Ok(ffi::pwm::PwmState {
            period_ns,
            duty_ns,
            enabled:  s.running,
            polarity: if s.invert {
                ffi::pwm::Polarity::Inversed
            } else {
                ffi::pwm::Polarity::Normal
            },
        })
    }
}

pub(crate) struct PwmChild {
    _handle: ffi::pwm::ChipHandle,
    #[allow(dead_code)]
    idx:     u16,
}

impl PwmChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state.clone());
        let mut pins = Vec::new();
        for &p in bus.pins.iter() { pins.push(p, GFP_KERNEL)?; }
        // Sensible default if the capability map didn't populate pins:
        // the rev-B firmware exposes PWM on 6 lines.
        if pins.is_empty() {
            for &p in &[30u8, 31, 37, 38, 39, 40] { pins.push(p, GFP_KERNEL)?; }
        }
        let npwm = pins.len() as u32;
        pr_info!("rpbridge-pwm.{}: {} channel(s)\n", bus.idx, npwm);

        let parent = crate::ffi::auxiliary::parent_device(
            &*core::ops::Deref::deref(&state)
        ).map_err(|e| kernel::error::Error::from_errno(e))?;

        let backend = PwmBackendImpl { client, pins };
        let handle = ffi::pwm::ChipHandle::register(parent, npwm, backend)
            .map_err(|e| kernel::error::Error::from_errno(e))?;
        Ok(PwmChild { _handle: handle, idx: bus.idx })
    }
}

/* ═════ Tests ═════════════════════════════════════════════════════ */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpbp::op::pwm as pw;

    #[test]
    fn encode_roundtrips_period_and_duty() {
        // 1 kHz, 50 % duty, no invert.
        let (freq, duty, inv) = encode_state(1_000_000, 500_000, false).unwrap();
        assert_eq!(freq, 1_000);
        assert!((0x7FFF..=0x8000).contains(&duty));
        assert!(!inv);
    }

    #[test]
    fn encode_full_duty_saturates() {
        let (_, duty, _) = encode_state(1_000_000, 1_000_000, false).unwrap();
        assert_eq!(duty, 65535);
    }

    #[test]
    fn encode_rejects_pathological_period() {
        assert!(encode_state(0, 0, false).is_err());
        assert!(encode_state(5, 5, false).is_err());
    }

    #[test]
    fn decode_reverses_encode_within_rounding() {
        let st = wire::PwmState {
            freq_hz: 1_000, duty_q16: 32_768,
            running: true, invert: false, slice: 0, channel: 0,
        };
        let (period_ns, duty_ns) = decode_state(&st);
        assert_eq!(period_ns, 1_000_000);
        // ~50 % of 1 ms → 500 000 ns ± 16 ns (Q0.16 rounding).
        assert!((duty_ns as i64 - 500_000).abs() <= 16);
    }

    #[test]
    fn build_configure_layout() {
        let mut buf = [0u8; 10];
        let n = wire::build_configure(&mut buf, 30, 0x8000, 1_000, true).unwrap();
        assert_eq!(n, 10);
        assert_eq!(buf[..10], [
            0x04, pw::CONFIGURE, 30, pw::FLAG_INVERT,
            0x00, 0x80,          // duty LE16
            0xE8, 0x03, 0x00, 0x00, // freq LE32 = 1000
        ]);
    }

    #[test]
    fn parse_state_extracts_all_fields() {
        let resp: [u8; 10] = [
            0xE8, 0x03, 0x00, 0x00,    // freq = 1000
            0x00, 0x80,                 // duty = 0x8000
            1, 1, 3, 2,                 // running, invert, slice, channel
        ];
        let st = wire::parse_state(&resp).unwrap();
        assert_eq!(st.freq_hz, 1000);
        assert_eq!(st.duty_q16, 0x8000);
        assert!(st.running);
        assert!(st.invert);
        assert_eq!(st.slice, 3);
        assert_eq!(st.channel, 2);
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
    fn apply_state_issues_configure_then_start() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]); // CONFIGURE
        mock.queue_response(Status::Ok, &[]); // START
        client.apply_state(30, 1_000_000, 500_000, false, true).unwrap();

        assert_eq!(mock.request_count(), 2);
        let cfg = mock.pop_request();
        let start = mock.pop_request();
        // CONFIGURE: subsys=4, op=0, pin=30
        assert_eq!(cfg.as_slice()[..3], [0x04, 0x00, 30]);
        // START: subsys=4, op=2, pin=30
        assert_eq!(start.as_slice(), &[0x04, 0x02, 30]);
    }

    #[test]
    fn apply_state_disabled_issues_stop_not_start() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]); // CONFIGURE
        mock.queue_response(Status::Ok, &[]); // STOP
        client.apply_state(30, 1_000_000, 500_000, false, false).unwrap();

        let _cfg = mock.pop_request();
        let stop = mock.pop_request();
        assert_eq!(stop.as_slice(), &[0x04, 0x03, 30]); // op=STOP
    }

    #[test]
    fn apply_state_forwards_ebusy_from_configure() {
        let (client, mock) = setup();
        mock.queue_response(Status::EBusy, &[]);
        let err = client.apply_state(30, 1_000_000, 500_000, false, true)
            .unwrap_err();
        assert_eq!(err, 16); // EBUSY
        // START should NOT have been queued — only CONFIGURE's request
        // was submitted before we bailed.
        assert_eq!(mock.request_count(), 1);
    }

    #[test]
    fn set_duty_q16_encoding() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        client.set_duty_q16(37, 0xABCD).unwrap();
        let req = mock.pop_request();
        // subsys=4, op=SET_DUTY (0x01), pin, duty LE16
        assert_eq!(req.as_slice()[..3], [0x04, 0x01, 37]);
        assert_eq!(&req.as_slice()[3..5], &[0xCD, 0xAB]);
    }

    #[test]
    fn start_stop_roundtrip() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        mock.queue_response(Status::Ok, &[]);
        client.start(30).unwrap();
        client.stop(30).unwrap();
        assert_eq!(mock.pop_request().as_slice(), &[0x04, 0x02, 30]);
        assert_eq!(mock.pop_request().as_slice(), &[0x04, 0x03, 30]);
    }

    #[test]
    fn get_state_parses_reply() {
        let (client, mock) = setup();
        let body: [u8; 10] = [
            0xE8, 0x03, 0x00, 0x00,
            0x00, 0x80,
            1, 0, 2, 1,
        ];
        mock.queue_response(Status::Ok, &body);
        let st = client.get_state(30).unwrap();
        assert_eq!(st.freq_hz, 1000);
        assert_eq!(st.duty_q16, 0x8000);
        assert!(st.running);
        assert!(!st.invert);
    }

    #[test]
    fn encode_state_invert_flag_propagates() {
        let (client, mock) = setup();
        mock.queue_response(Status::Ok, &[]);
        mock.queue_response(Status::Ok, &[]);
        client.apply_state(30, 1_000_000, 250_000, true, true).unwrap();
        let cfg = mock.pop_request();
        // flags byte is at offset 3 (after subsys/op/pin).
        assert_eq!(cfg.as_slice()[3], crate::rpbp::op::pwm::FLAG_INVERT);
    }
}
