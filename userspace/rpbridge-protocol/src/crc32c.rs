// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// CRC-32C (Castagnoli, polynomial 0x1EDC6F41), reflected, init 0xFFFFFFFF,
// final XOR 0xFFFFFFFF. Matches:
//   - the firmware's firmware/src/rpbp/rpbp_crc32c.c
//   - the kernel's libcrc32c / <linux/crc32c.h>
//   - Intel SSE4.2 `crc32` and ARMv8 `CRC32CX`
//   - RFC 3720 §B.4 iSCSI CRC test vectors
//
// A single 256-entry table is built at compile time. We do not bring in
// a dependency on the `crc32c` crate to stay `no_std`-only and to keep
// the wire-format implementation close to the byte layout.

/// Initial accumulator for CRC-32C streaming updates.
pub const CRC32C_INIT: u32 = 0xFFFF_FFFF;

const POLY_REFLECTED: u32 = 0x82F6_3B78;

const TABLE: [u32; 256] = {
    let mut t = [0u32; 256];
    let mut i: u32 = 0;
    while i < 256 {
        let mut c = i;
        let mut k = 0;
        while k < 8 {
            c = if (c & 1) != 0 {
                (c >> 1) ^ POLY_REFLECTED
            } else {
                c >> 1
            };
            k += 1;
        }
        t[i as usize] = c;
        i += 1;
    }
    t
};

/// Update a running CRC-32C accumulator with `buf`. Feed the result of
/// the previous call as `crc`; initialise with [`CRC32C_INIT`] for a
/// fresh stream. The returned value is NOT the final CRC — use
/// [`crc32c`] to compute the full CRC of a single contiguous slice.
#[inline]
#[must_use]
pub fn crc32c_update(mut crc: u32, buf: &[u8]) -> u32 {
    for &b in buf {
        let idx = ((crc ^ u32::from(b)) & 0xFF) as usize;
        crc = (crc >> 8) ^ TABLE[idx];
    }
    crc
}

/// Final CRC-32C of a single contiguous slice. Equivalent to the C
/// function `crc32c(buf, len)` in the firmware.
#[inline]
#[must_use]
pub fn crc32c(buf: &[u8]) -> u32 {
    crc32c_update(CRC32C_INIT, buf) ^ 0xFFFF_FFFF
}

#[cfg(test)]
mod tests {
    use super::{crc32c, crc32c_update, CRC32C_INIT};

    #[test]
    fn empty_is_zero_after_xor() {
        assert_eq!(crc32c(&[]), 0);
    }

    #[test]
    fn rfc3720_vector() {
        // RFC 3720 §B.4 — CRC-32C of ASCII "123456789" is 0xE3069283
        assert_eq!(crc32c(b"123456789"), 0xE306_9283);
    }

    #[test]
    fn streaming_matches_oneshot() {
        let data = b"the quick brown fox jumps over the lazy dog";
        let oneshot = crc32c(data);

        let mut running = CRC32C_INIT;
        for chunk in data.chunks(7) {
            running = crc32c_update(running, chunk);
        }
        assert_eq!(running ^ 0xFFFF_FFFF, oneshot);
    }

    #[test]
    fn single_byte_vector() {
        // One-byte CRC-32C of 0xFE is 0x0D6B8303. Cross-checked
        // against an independent reference implementation (reflected
        // poly 0x82F63B78, init/xor-out 0xFFFFFFFF) and against
        // Python's `crc32c` package on the host.
        assert_eq!(crc32c(&[0xFE]), 0x0D6B_8303);
    }
}
