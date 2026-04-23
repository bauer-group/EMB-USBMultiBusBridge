# rpbridge — high-level Rust client library

Ergonomic Rust API for talking to an RPBridge USB multi-bus bridge.
Wraps `rpbridge-protocol` (framing) with a typed view over each
subsystem and hides the USB transport behind a feature flag:

* `nusb-backend` (default) — pure-Rust, async-friendly.
* `rusb-backend` — libusb-based, useful on platforms where libusb is
  already a system-integrated dependency.

```rust,no_run
use rpbridge::Device;

fn main() -> rpbridge::Result<()> {
    let dev = Device::open_first()?;
    let caps = dev.capabilities();
    println!("connected to {}", caps.board.unwrap_or_default());
    Ok(())
}
```

This crate is currently a scaffold — the `Device::open_first()` body
returns an informative error. Implementation is tracked in the
workspace `TODO` and the protocol roadmap.
