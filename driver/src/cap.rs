// SPDX-License-Identifier: GPL-2.0
//
// Capability-map parser. The device answers
// `CMD_REQUEST{GET_CAPABILITIES}` with a CBOR map documented in
// docs/protocol/capabilities.md. Keys we recognise:
//
//     proto           : [u8, u8, u8]
//     fw              : text
//     board           : text
//     serial          : bytes
//     mtu             : { "out": u, "in": u }
//     max_msg_len     : u
//     max_streams     : u
//     max_reassembled : u
//     features        : [text ...]
//     buses           : { "uart": [..], "i2c": [..], "spi": [..],
//                         "gpio": [..], "pwm": [..], "adc": [..],
//                         "can":  [..] }
//     access          : { <subsys> : "cdc" | "vendor" | ... }
//     ota             : { mechanism: text, signature: text }
//
// The parser tolerates unknown keys (forward compatibility) but
// rejects malformed CBOR structurally. Cap at 4 KiB input — matches
// RPBP_MAX_PAYLOAD_LEN.

use kernel::prelude::*;

/* ───── Minimal CBOR walker (RFC 8949 §3) ────────────────────────── */

/// Major type as encoded in the top 3 bits of the initial byte.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum Major {
    Uint = 0,
    NegInt = 1,
    Bytes = 2,
    Text = 3,
    Array = 4,
    Map = 5,
    Tag = 6,
    Simple = 7,
}

struct Cbor<'a> { buf: &'a [u8], pos: usize }

impl<'a> Cbor<'a> {
    fn new(buf: &'a [u8]) -> Self { Self { buf, pos: 0 } }

    fn take(&mut self, n: usize) -> Result<&'a [u8], i32> {
        if self.pos + n > self.buf.len() { return Err(crate::errno::EPROTO); }
        let s = &self.buf[self.pos..self.pos + n];
        self.pos += n;
        Ok(s)
    }

    fn read_head(&mut self) -> Result<(Major, u64), i32> {
        let b = *self.take(1)?.first().ok_or(crate::errno::EPROTO)?;
        let mt_num = b >> 5;
        let info = b & 0x1F;
        let major = match mt_num {
            0 => Major::Uint,   1 => Major::NegInt, 2 => Major::Bytes,
            3 => Major::Text,   4 => Major::Array,  5 => Major::Map,
            6 => Major::Tag,    7 => Major::Simple,
            _ => return Err(crate::errno::EPROTO),
        };
        let val = match info {
            0..=23 => u64::from(info),
            24 => u64::from(self.take(1)?[0]),
            25 => {
                let s = self.take(2)?;
                u64::from(u16::from_be_bytes([s[0], s[1]]))
            }
            26 => {
                let s = self.take(4)?;
                u64::from(u32::from_be_bytes([s[0], s[1], s[2], s[3]]))
            }
            27 => {
                let s = self.take(8)?;
                u64::from_be_bytes([s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7]])
            }
            28..=30 | 31 => return Err(crate::errno::EPROTO), // no indefinite
            _ => unreachable!(),
        };
        Ok((major, val))
    }

    /// Expect a text string; return its bytes.
    fn text(&mut self) -> Result<&'a [u8], i32> {
        let (m, n) = self.read_head()?;
        if m != Major::Text { return Err(crate::errno::EPROTO); }
        self.take(n as usize)
    }

    /// Expect an unsigned int fitting in u64.
    fn uint(&mut self) -> Result<u64, i32> {
        let (m, n) = self.read_head()?;
        if m != Major::Uint { return Err(crate::errno::EPROTO); }
        Ok(n)
    }

    /// Skip over the next item (value of any type) — used to tolerate
    /// unknown keys.
    fn skip(&mut self) -> Result<(), i32> {
        let (m, n) = self.read_head()?;
        match m {
            Major::Uint | Major::NegInt | Major::Simple => Ok(()),
            Major::Tag => self.skip(),
            Major::Bytes | Major::Text => {
                self.take(n as usize)?;
                Ok(())
            }
            Major::Array => {
                for _ in 0..n { self.skip()?; }
                Ok(())
            }
            Major::Map => {
                for _ in 0..n { self.skip()?; self.skip()?; }
                Ok(())
            }
        }
    }
}

/* ───── Public types ─────────────────────────────────────────────── */

/// Per-bus descriptor extracted from the capability map. Fields are
/// populated on a best-effort basis from whatever the device sent;
/// unknown fields are ignored.
#[derive(Debug, Default, Clone)]
pub struct BusCaps {
    pub class:       KString,   // "uart" | "i2c" | "spi" | "gpio" | "pwm" | "adc" | "can"
    pub idx:         u16,
    pub pins:        Vec<u8>,   // whitelist / bus pins, class-dependent
    pub max_freq_hz: u32,
    pub access:      KString,   // "cdc" | "vendor" | "cdc+vendor"
}

#[derive(Debug, Default)]
pub struct Capabilities {
    pub proto:          (u8, u8, u8),
    pub fw_version:     KString,
    pub board:          KString,
    pub mtu_out:        u16,
    pub mtu_in:         u16,
    pub max_msg_len:    u32,
    pub max_streams:    u16,
    pub features:       Vec<KString>,
    pub buses:          Vec<BusCaps>,
}

impl Capabilities {
    /// Parse a CBOR byte slice into a `Capabilities` value.
    pub fn from_cbor(src: &[u8]) -> Result<Self, i32> {
        if src.len() > crate::rpbp::MAX_PAYLOAD_LEN {
            return Err(crate::errno::EPROTO);
        }
        let mut p = Cbor::new(src);
        let (m, n) = p.read_head()?;
        if m != Major::Map { return Err(crate::errno::EPROTO); }

        let mut out = Capabilities::default();
        let mut access_uart: KString = KString::new();
        let mut access_i2c:  KString = KString::new();
        let mut access_spi:  KString = KString::new();
        let mut access_gpio: KString = KString::new();
        let mut access_pwm:  KString = KString::new();
        let mut access_adc:  KString = KString::new();
        let mut access_can:  KString = KString::new();

        for _ in 0..n {
            let key_bytes = p.text()?;
            let key = core::str::from_utf8(key_bytes)
                        .map_err(|_| crate::errno::EPROTO)?;
            match key {
                "proto" => {
                    let (am, an) = p.read_head()?;
                    if am != Major::Array || an != 3 { return Err(crate::errno::EPROTO); }
                    let a = p.uint()?;
                    let b = p.uint()?;
                    let c = p.uint()?;
                    out.proto = (a as u8, b as u8, c as u8);
                }
                "fw"    => { out.fw_version = kstring_from_cbor_text(p.text()?)?; }
                "board" => { out.board      = kstring_from_cbor_text(p.text()?)?; }
                "max_msg_len" => out.max_msg_len = p.uint()? as u32,
                "max_streams" => out.max_streams = p.uint()? as u16,
                "mtu" => {
                    let (mm, mn) = p.read_head()?;
                    if mm != Major::Map { return Err(crate::errno::EPROTO); }
                    for _ in 0..mn {
                        let k = p.text()?;
                        let v = p.uint()?;
                        match core::str::from_utf8(k).unwrap_or("") {
                            "out" => out.mtu_out = v as u16,
                            "in"  => out.mtu_in  = v as u16,
                            _     => {}
                        }
                    }
                }
                "features" => {
                    let (fm, fn_) = p.read_head()?;
                    if fm != Major::Array { return Err(crate::errno::EPROTO); }
                    for _ in 0..fn_ {
                        out.features.push(kstring_from_cbor_text(p.text()?)?, GFP_KERNEL)?;
                    }
                }
                "buses" => {
                    let (bm, bn) = p.read_head()?;
                    if bm != Major::Map { return Err(crate::errno::EPROTO); }
                    for _ in 0..bn {
                        let class_bytes = p.text()?;
                        let class = kstring_from_cbor_text(class_bytes)?;
                        // Each class maps to an array of per-instance maps.
                        let (am, an) = p.read_head()?;
                        if am != Major::Array { return Err(crate::errno::EPROTO); }
                        for _ in 0..an {
                            let mut bus = BusCaps {
                                class: class.clone(),
                                ..Default::default()
                            };
                            let (im, in_) = p.read_head()?;
                            if im != Major::Map { return Err(crate::errno::EPROTO); }
                            for _ in 0..in_ {
                                let k = p.text()?;
                                match core::str::from_utf8(k).unwrap_or("") {
                                    "idx"      => bus.idx = p.uint()? as u16,
                                    "max_freq" | "max_bitrate_bps" => bus.max_freq_hz = p.uint()? as u32,
                                    "pins" | "cs_pins" => {
                                        let (pm, pn) = p.read_head()?;
                                        if pm != Major::Array { return Err(crate::errno::EPROTO); }
                                        for _ in 0..pn {
                                            bus.pins.push(p.uint()? as u8, GFP_KERNEL)?;
                                        }
                                    }
                                    _ => p.skip()?, // tolerate unknown sub-keys
                                }
                            }
                            out.buses.push(bus, GFP_KERNEL)?;
                        }
                    }
                }
                "access" => {
                    let (am, an) = p.read_head()?;
                    if am != Major::Map { return Err(crate::errno::EPROTO); }
                    for _ in 0..an {
                        let k = p.text()?;
                        let v = kstring_from_cbor_text(p.text()?)?;
                        match core::str::from_utf8(k).unwrap_or("") {
                            "uart" => access_uart = v,
                            "i2c"  => access_i2c  = v,
                            "spi"  => access_spi  = v,
                            "gpio" => access_gpio = v,
                            "pwm"  => access_pwm  = v,
                            "adc"  => access_adc  = v,
                            "can"  => access_can  = v,
                            _ => {}
                        }
                    }
                }
                _ => p.skip()?, // unknown top-level key: tolerate
            }
        }

        // Fold per-class access strings into each bus entry so callers
        // downstream don't need a second lookup.
        for bus in out.buses.iter_mut() {
            let src = match bus.class.as_bytes() {
                b"uart" => &access_uart,
                b"i2c"  => &access_i2c,
                b"spi"  => &access_spi,
                b"gpio" => &access_gpio,
                b"pwm"  => &access_pwm,
                b"adc"  => &access_adc,
                b"can"  => &access_can,
                _       => continue,
            };
            bus.access = src.clone();
        }

        Ok(out)
    }

    /// Convenience: return every bus with the given class name.
    pub fn buses_of_class(&self, class: &str) -> impl Iterator<Item = &BusCaps> {
        self.buses.iter().filter(move |b| b.class.as_bytes() == class.as_bytes())
    }

    /// Whether a specific feature flag is advertised.
    pub fn has_feature(&self, name: &str) -> bool {
        self.features.iter().any(|f| f.as_bytes() == name.as_bytes())
    }
}

/// Helper: CBOR text bytes -> KString, fallible on OOM or non-UTF-8.
fn kstring_from_cbor_text(bytes: &[u8]) -> Result<KString, i32> {
    let s = core::str::from_utf8(bytes).map_err(|_| crate::errno::EPROTO)?;
    KString::try_from(s).map_err(|_| crate::errno::ENOMEM)
}
