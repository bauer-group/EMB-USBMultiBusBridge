# RPBridge

**USB Multi-Bus Bridge on Raspberry Pi RP2354B.** Open hardware, open
firmware, open drivers. Exposes I²C, SPI, UART (RS232/RS485/TTL),
GPIO, PWM, ADC and CAN 2.0B to a Linux host as standard subsystem
devices — use `i2c-tools`, `libgpiod`, `can-utils`, `spidev_test` and
friends unmodified.

## Start here

<div class="grid cards" markdown>

-   :material-rocket-launch: __Quickstart__

    ---

    From a clean workstation to a working device in under five minutes:

    [:octicons-arrow-right-24: Quickstart](tutorials/quickstart.md)

-   :material-protocol: __Protocol — RPBP v1__

    ---

    Wire format, framing, CRC-32C, capability discovery. The single
    canonical source for what goes over the bulk endpoint.

    [:octicons-arrow-right-24: Specification](protocol/rpbp-v1.md)

-   :material-chip: __Firmware__

    ---

    pico-sdk 2.2.0, TinyUSB, can2040 — how the device side is built
    and flashed.

    [:octicons-arrow-right-24: Firmware guide](firmware/overview.md)

-   :material-linux: __Linux driver__

    ---

    Rust-for-Linux module, `auxiliary_bus` children, GPIO cdev v2.
    DKMS packages for Debian/Ubuntu/Fedora.

    [:octicons-arrow-right-24: Driver guide](driver/overview.md)

</div>

## Repository layout

| Path                   | Purpose                                                     |
|------------------------|-------------------------------------------------------------|
| `firmware/`            | RP2354B firmware (C + pico-sdk, can2040 for CAN).           |
| `driver/`              | Rust-for-Linux kernel module, DKMS-packaged.                |
| `userspace/`           | Rust workspace: protocol lib, client lib, `rpbridge-ctl`, bench. |
| `docs/`                | This documentation site (mkdocs-material).                  |
| `dts/overlays/`        | Device-tree overlays for auto-provisioning onboard sensors. |
| `hardware/`            | KiCad 9 design files (rev-B, 2-layer, 80 × 55 mm).          |
| `tests/golden/`        | Golden RPBP vectors shared across every implementation.     |

## Design principles

1. **Self-describing device.** The host reads a CBOR capability map at
   bind time — no firmware-version matrices to maintain.
2. **Standard Linux subsystems.** `/dev/i2c-N`, `/dev/spidevN.M`,
   `/dev/gpiochipN`, `/dev/ttyACM*`, `can0`, IIO — no private character
   devices for bus access.
3. **Modern stack only.** `auxiliary_bus` (≥ 5.11), GPIO cdev v2,
   libgpiod 2.x, SocketCAN. Deprecated paths are intentionally not
   implemented.
4. **Forward-/backward-compatible.** Four independent SemVer axes
   (protocol, firmware, driver, userspace); negotiated at runtime
   through feature flags.

## Status

| Component           | Maturity      |
|---------------------|---------------|
| Hardware (rev-B)    | Draft         |
| Firmware            | Scaffold      |
| Linux driver (Rust) | Scaffold      |
| Userspace CLI (Rust)| Scaffold      |
| Documentation       | In progress   |
| CI/CD               | Scaffold      |

See [CHANGELOG](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/CHANGELOG.md)
for the running release log and [decision records](adr/index.md) for
why the project shaped up the way it did.
