// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-spi — RPBP SPI subsystem client + auxiliary spi_controller
// child. Wire format in docs/protocol/subsystems/spi.md.

use kernel::prelude::*;
use kernel::sync::Arc;

use crate::cap::BusCaps;
use crate::core_driver::RpbridgeState;
use crate::rpbp::{CHAN_CONTROL, MAX_PAYLOAD_LEN, Status};
use crate::transport::RoundTrip;

/* ═════ Layer 1 — wire ════════════════════════════════════════════ */

pub mod wire {
    use kernel::prelude::*;
    use crate::rpbp::{self, op::spi as sp};

    fn start(dst: &mut [u8], opcode: u8) -> Result<usize, i32> {
        if dst.len() < 2 { return Err(crate::errno::EINVAL); }
        dst[0] = rpbp::Subsys::Spi as u8;
        dst[1] = opcode;
        Ok(2)
    }

    /* 0x00 XFER — args `[inst][cs_pin][flags][rsv][tx_len LE16][rx_len LE16][tx…]` */
    pub fn build_xfer(
        dst: &mut [u8], instance: u8, cs_pin: u8, flags: u8,
        tx: &[u8], rx_len: u16,
    ) -> Result<usize, i32> {
        let n = start(dst, sp::XFER)?;
        let need = n + 8 + tx.len();
        if dst.len() < need { return Err(crate::errno::EINVAL); }
        dst[n]     = instance;
        dst[n + 1] = cs_pin;
        dst[n + 2] = flags;
        dst[n + 3] = 0;
        rpbp::wr_le16(&mut dst[n + 4..], tx.len() as u16);
        rpbp::wr_le16(&mut dst[n + 6..], rx_len);
        dst[n + 8..n + 8 + tx.len()].copy_from_slice(tx);
        Ok(need)
    }

    pub fn parse_xfer<'a>(resp: &'a [u8]) -> Result<&'a [u8], i32> {
        if resp.len() < 2 { return Err(crate::errno::EPROTO); }
        let rx = rpbp::rd_le16(&resp[..2]) as usize;
        if resp.len() < 2 + rx { return Err(crate::errno::EPROTO); }
        Ok(&resp[2..2 + rx])
    }

    /* 0x01 SET_MODE — [inst][mode_bits] */
    pub fn build_set_mode(dst: &mut [u8], inst: u8, mode_bits: u8) -> Result<usize, i32> {
        let n = start(dst, sp::SET_MODE)?;
        if dst.len() < n + 2 { return Err(crate::errno::EINVAL); }
        dst[n] = inst;
        dst[n + 1] = mode_bits;
        Ok(n + 2)
    }

    /* 0x02 SET_FREQ / 0x03 GET_FREQ — [inst][freq LE32] / [inst] */
    pub fn build_set_freq(dst: &mut [u8], inst: u8, hz: u32) -> Result<usize, i32> {
        let n = start(dst, sp::SET_FREQ)?;
        if dst.len() < n + 5 { return Err(crate::errno::EINVAL); }
        dst[n] = inst;
        rpbp::wr_le32(&mut dst[n + 1..], hz);
        Ok(n + 5)
    }

    pub fn build_get_freq(dst: &mut [u8], inst: u8) -> Result<usize, i32> {
        let n = start(dst, sp::GET_FREQ)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = inst;
        Ok(n + 1)
    }

    pub fn parse_freq(resp: &[u8]) -> Result<u32, i32> {
        if resp.len() < 4 { return Err(crate::errno::EPROTO); }
        Ok(rpbp::rd_le32(&resp[..4]))
    }

    /* 0x04 CS_ASSERT / 0x05 CS_RELEASE — [cs_pin] */
    pub fn build_cs(dst: &mut [u8], cs_pin: u8, assert: bool) -> Result<usize, i32> {
        let op = if assert { sp::CS_ASSERT } else { sp::CS_RELEASE };
        let n = start(dst, op)?;
        if dst.len() < n + 1 { return Err(crate::errno::EINVAL); }
        dst[n] = cs_pin;
        Ok(n + 1)
    }
}

/* ═════ Layer 2 — Client ══════════════════════════════════════════ */

/// Canonical SPI mode as encoded by the kernel (CPOL | CPHA).
/// Mapped to the firmware's MODE_CPHA/MODE_CPOL bits in set_mode().
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Mode { Mode0 = 0, Mode1 = 1, Mode2 = 2, Mode3 = 3 }

pub struct Client<T: RoundTrip + ?Sized> {
    rt:       Arc<T>,
    instance: u8,
}

impl<T: RoundTrip + ?Sized> Client<T> {
    pub fn new(rt: Arc<T>, instance: u8) -> Self { Self { rt, instance } }

    fn rt_call(&self, req: &[u8]) -> Result<KVec<u8>, i32> {
        let (status, payload) = self.rt.round_trip(CHAN_CONTROL, req)?;
        if !matches!(status, Status::Ok) { return Err(status.to_errno()); }
        Ok(payload)
    }

    pub fn set_mode(&self, mode: Mode) -> Result<(), i32> {
        use crate::rpbp::op::spi as sp;
        let bits = match mode {
            Mode::Mode0 => 0,
            Mode::Mode1 => sp::MODE_CPHA,
            Mode::Mode2 => sp::MODE_CPOL,
            Mode::Mode3 => sp::MODE_CPHA | sp::MODE_CPOL,
        };
        let mut buf = [0u8; 4];
        let n = wire::build_set_mode(&mut buf, self.instance, bits)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn set_freq(&self, hz: u32) -> Result<(), i32> {
        let mut buf = [0u8; 7];
        let n = wire::build_set_freq(&mut buf, self.instance, hz)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn get_freq(&self) -> Result<u32, i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_get_freq(&mut buf, self.instance)?;
        let resp = self.rt_call(&buf[..n])?;
        wire::parse_freq(resp.as_slice())
    }

    pub fn cs_assert(&self, cs_pin: u8) -> Result<(), i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_cs(&mut buf, cs_pin, true)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    pub fn cs_release(&self, cs_pin: u8) -> Result<(), i32> {
        let mut buf = [0u8; 3];
        let n = wire::build_cs(&mut buf, cs_pin, false)?;
        let _ = self.rt_call(&buf[..n])?;
        Ok(())
    }

    /// Execute one SPI transfer. Setting `hold_cs=true` corresponds
    /// to the kernel's `spi_transfer.cs_change = 0` — CS stays
    /// asserted past this transfer so the next one can follow
    /// without a STOP. The last transfer of a message always has
    /// `hold_cs=false`.
    ///
    /// Returns the RX bytes (always `rx_len` long on success).
    pub fn xfer(
        &self, cs_pin: u8, tx: &[u8], rx_len: u16, hold_cs: bool,
    ) -> Result<KVec<u8>, i32> {
        let flags = if hold_cs { crate::rpbp::op::spi::FLAG_HOLD_CS } else { 0 };
        // header + 8-byte xfer args; keep inside RPBP payload ceiling.
        const MARGIN: usize = 16;
        if tx.len() + MARGIN > MAX_PAYLOAD_LEN {
            return Err(crate::errno::EINVAL);
        }
        let mut buf = KVec::new();
        buf.reserve(tx.len() + MARGIN, GFP_KERNEL)?;
        for _ in 0..(tx.len() + MARGIN) { buf.push(0, GFP_KERNEL)?; }
        let n = wire::build_xfer(
            buf.as_mut_slice(), self.instance, cs_pin, flags, tx, rx_len,
        )?;
        let resp = self.rt_call(&buf.as_slice()[..n])?;
        let rx = wire::parse_xfer(resp.as_slice())?;
        let mut out = KVec::new();
        out.extend_from_slice(rx, GFP_KERNEL)
            .map_err(|_| crate::errno::ENOMEM)?;
        Ok(out)
    }

    /// Multi-transfer dispatcher that matches the kernel's
    /// `spi_message` shape. Each item in `xfers` is
    /// `(tx, rx_len, cs_change)` — `cs_change=true` means "release
    /// CS after this xfer" (the kernel's cs_change=1 convention).
    pub fn xfer_message<'a>(
        &self, cs_pin: u8, xfers: &'a [(&'a [u8], u16, bool)],
    ) -> Result<Vec<KVec<u8>>, i32> {
        let mut out = Vec::new();
        out.reserve(xfers.len(), GFP_KERNEL)?;
        let last = xfers.len().saturating_sub(1);
        for (i, (tx, rx_len, cs_change)) in xfers.iter().enumerate() {
            // Hold CS unless this xfer asked for a release or is the
            // final transfer of the message.
            let hold_cs = !*cs_change && i != last;
            let rx = self.xfer(cs_pin, tx, *rx_len, hold_cs)?;
            out.push(rx, GFP_KERNEL)?;
        }
        Ok(out)
    }
}

/* ═════ Layer 3 — auxiliary_device adapter ════════════════════════ */

#[allow(dead_code)]
pub(crate) struct SpiChild {
    client: Client<RpbridgeState>,
    #[allow(dead_code)]
    idx:    u16,
}

impl SpiChild {
    pub(crate) fn probe(
        state: Arc<RpbridgeState>, bus: &BusCaps,
    ) -> Result<Self> {
        let client = Client::new(state, bus.idx as u8);
        pr_info!("rpbridge-spi.{}: probe (client ready)\n", bus.idx);

        // TODO §R4L-SPI-BINDINGS:
        //
        //   Once kernel::spi::Controller lands:
        //
        //     fn transfer_one_message(&self, msg: &SpiMessage) -> Result<()> {
        //         let xfers: Vec<(&[u8], u16, bool)> = msg.xfers()
        //             .map(|x| (x.tx_buf(), x.rx_len() as u16, x.cs_change())).collect();
        //         let cs_pin = msg.chip_select_gpio();
        //         let replies = self.client.xfer_message(cs_pin, &xfers)?;
        //         for (xfer, rx) in msg.xfers().zip(replies) {
        //             if let Some(dst) = xfer.rx_buf_mut() {
        //                 dst.copy_from_slice(&rx);
        //             }
        //         }
        //         msg.complete(0);
        //         Ok(())
        //     }
        //
        //     .num_chipselect = bus.pins.len()
        //     .mode_bits      = SPI_CPOL | SPI_CPHA
        //     .bits_per_word_mask = BIT(8)

        Ok(SpiChild { client, idx: bus.idx })
    }

    #[allow(dead_code)]
    pub(crate) fn client(&self) -> &Client<RpbridgeState> { &self.client }
}

/* ═════ Tests ═════════════════════════════════════════════════════ */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpbp::op::spi as sp;

    #[test]
    fn build_xfer_layout() {
        let mut buf = [0u8; 20];
        let n = wire::build_xfer(&mut buf, 1, 30, sp::FLAG_HOLD_CS,
                                 &[0xAB, 0xCD], 3).unwrap();
        assert_eq!(n, 12);
        // [subsys=2][op=XFER][inst=1][cs=30][flags][rsv=0][tx_len LE16][rx_len LE16][tx...]
        assert_eq!(buf[..12], [
            0x02, sp::XFER, 1, 30, sp::FLAG_HOLD_CS, 0x00,
            0x02, 0x00, 0x03, 0x00, 0xAB, 0xCD,
        ]);
    }

    #[test]
    fn set_mode_encoding_matches_canonical() {
        // Mode 3 = CPOL | CPHA. Build SET_MODE and inspect the bits.
        let mut buf = [0u8; 4];
        let n = wire::build_set_mode(&mut buf, 0, sp::MODE_CPOL | sp::MODE_CPHA).unwrap();
        assert_eq!(n, 4);
        assert_eq!(buf[3], sp::MODE_CPOL | sp::MODE_CPHA);
    }

    #[test]
    fn set_freq_encodes_le32() {
        let mut buf = [0u8; 7];
        let n = wire::build_set_freq(&mut buf, 1, 8_000_000).unwrap();
        assert_eq!(n, 7);
        assert_eq!(buf[3..7], 8_000_000u32.to_le_bytes());
    }

    #[test]
    fn parse_freq_le32() {
        let resp = 24_000_000u32.to_le_bytes();
        assert_eq!(wire::parse_freq(&resp).unwrap(), 24_000_000);
    }

    #[test]
    fn parse_xfer_rx_length_matches_field() {
        // reply: [rx_len=4 LE16][rx_data 4 bytes]
        let resp = [0x04, 0x00, 0xDE, 0xAD, 0xBE, 0xEF];
        assert_eq!(wire::parse_xfer(&resp).unwrap(), &[0xDE, 0xAD, 0xBE, 0xEF]);
    }
}
