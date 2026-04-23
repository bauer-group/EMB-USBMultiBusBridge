# rpbridge-protocol

Canonical Rust implementation of the **RPBridge Protocol, v1 (RPBP)**.
The crate is `no_std`-capable (enable the `std` feature for
convenience on hosts), zero-allocation on the hot path, and exposes:

* Wire-format constants and enums that mirror the firmware's
  `firmware/src/rpbp/rpbp.h` byte-for-byte.
* `Header::parse()` / `Header::encode()` — deterministic, no panics on
  malformed input.
* `validate_frame()` — full frame check with trailing CRC-32C.
* `crc32c()` — Castagnoli polynomial, identical to the firmware's
  `rpbp_crc32c()` and the kernel's `libcrc32c.ko`.
* `cap` module (behind the `cbor` feature) — helpers for reading and
  emitting the capability map.

## Status

Draft — scaffold only. The byte layout and status codes are locked by
the v1 specification in
[`docs/protocol/rpbp-v1.md`](../../docs/protocol/rpbp-v1.md); new
helpers land as the firmware and kernel driver exercise them.

## Testing

```sh
cargo test
cargo test --no-default-features       # confirms no_std buildability
cargo test --features cbor
```

The golden-vector corpus lives at [`tests/golden/`](../../tests/golden/)
and is shared across every language binding.
