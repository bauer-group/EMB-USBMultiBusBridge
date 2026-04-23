// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// CBOR capability-map helpers. Gated behind the `cbor` feature so
// `no_std` consumers (kernel driver if it pulls this crate, or an
// embedded host) can skip the `ciborium` dependency.

use ciborium::Value;

/// Minimal typed view of the capability map returned by the device in
/// response to a `CmdRequest{ GET_CAPABILITIES }`. Fields are extracted
/// best-effort — unknown keys are ignored, missing keys yield `None`.
#[derive(Debug, Default, Clone)]
pub struct Capabilities {
    /// `(MAJOR, MINOR, PATCH)` of the RPBP protocol the device speaks.
    pub proto: Option<(u8, u8, u8)>,
    /// Firmware semantic version (`"1.2.0+abc123"`).
    pub firmware: Option<String>,
    /// Board identifier (`"rpbridge-rev-B"`).
    pub board: Option<String>,
    /// Negotiated MTU for host → device transfers.
    pub mtu_out: Option<u16>,
    /// Negotiated MTU for device → host transfers.
    pub mtu_in: Option<u16>,
}

impl Capabilities {
    /// Decode a CBOR capability map from a byte slice.
    ///
    /// # Errors
    ///
    /// Returns the underlying `ciborium` error if the input is not
    /// valid CBOR.
    pub fn from_cbor(src: &[u8]) -> Result<Self, ciborium::de::Error<std::io::Error>> {
        let val: Value = ciborium::de::from_reader(src)?;
        Ok(Self::from_value(&val))
    }

    fn from_value(val: &Value) -> Self {
        let Value::Map(map) = val else {
            return Capabilities::default();
        };
        let mut c = Capabilities::default();
        for (k, v) in map {
            let key = match k {
                Value::Text(s) => s.as_str(),
                _ => continue,
            };
            match (key, v) {
                ("proto", Value::Array(a)) if a.len() == 3 => {
                    let triple = (as_u8(&a[0]), as_u8(&a[1]), as_u8(&a[2]));
                    if let (Some(a), Some(b), Some(c_)) = triple {
                        c.proto = Some((a, b, c_));
                    }
                }
                ("fw", Value::Text(s)) => c.firmware = Some(s.clone()),
                ("board", Value::Text(s)) => c.board = Some(s.clone()),
                ("mtu", Value::Map(inner)) => {
                    for (ik, iv) in inner {
                        if let Value::Text(s) = ik {
                            if let Some(n) = as_u16(iv) {
                                match s.as_str() {
                                    "out" => c.mtu_out = Some(n),
                                    "in" => c.mtu_in = Some(n),
                                    _ => {}
                                }
                            }
                        }
                    }
                }
                _ => {}
            }
        }
        c
    }
}

fn as_u8(v: &Value) -> Option<u8> {
    match v {
        Value::Integer(i) => i128::from(*i).try_into().ok(),
        _ => None,
    }
}

fn as_u16(v: &Value) -> Option<u16> {
    match v {
        Value::Integer(i) => i128::from(*i).try_into().ok(),
        _ => None,
    }
}
