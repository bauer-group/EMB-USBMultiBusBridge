# RPBridge — Userspace (Rust)

Cargo workspace containing four crates that together form the
user-facing toolchain for the RPBridge bus bridge:

| Crate                | Role                                                                  |
|----------------------|-----------------------------------------------------------------------|
| `rpbridge-protocol`  | RPBP v1 framing + CRC-32C, `no_std`-capable. The single canonical   |
|                      | wire-format implementation — kernel driver, firmware host tests and |
|                      | userspace all agree because they pull from this crate (or mirror it).|
| `rpbridge`           | High-level client library: USB transport (via `nusb`), capability    |
|                      | discovery, per-bus handles (I²C, SPI, GPIO, PWM, ADC, UART phy).    |
| `rpbridge-ctl`       | The `rpbridge-ctl` CLI: `info`, `flash`, `selftest`, `i2c scan`,    |
|                      | `gpio set`, `adc read`, `reboot-bootsel`, `log --follow`, …         |
| `rpbridge-bench`     | Latency + throughput benchmarking harness for regression tracking.  |

## Quick start

```sh
cd userspace
cargo build --release                  # x86_64-unknown-linux-gnu
cargo run --release -p rpbridge-ctl -- info
cargo run --release -p rpbridge-ctl -- i2c --bus 0 scan
cargo run --release -p rpbridge-ctl -- flash ../firmware/build/rpbridge.uf2
```

Cross-compile for a Raspberry Pi or an ARM SBC:

```sh
cargo build --release --target aarch64-unknown-linux-gnu -p rpbridge-ctl
```

(Install the cross linker with `sudo apt install gcc-aarch64-linux-gnu`
and the target via `rustup target add aarch64-unknown-linux-gnu`.)

## Testing

```sh
cargo fmt --all -- --check
cargo clippy --all-targets --all-features -- -D warnings
cargo test --all-features
```

`rpbridge-protocol` carries the bulk of tests — its functions are pure
and the golden-vector corpus under `../tests/golden/` drives property
+ example tests that round-trip every documented frame through the
parser.
