# RPBridge

!!! warning "Alpha — pre-production"
    RPBridge is in **alpha**. The firmware feature set is complete and
    self-tested on both supported boards, but the project still uses
    placeholder USB VID/PID, the Rust-for-Linux kernel adapters are
    gated behind `CONFIG_RPBRIDGE_*`, and the udev rule set is not yet
    final. **Do not deploy on production hosts.** See
    [`firmware/COVERAGE.md`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/firmware/COVERAGE.md)
    for the live status matrix.

**USB Multi-Bus Bridge on the Raspberry Pi RP2350 family.** Open
hardware, open firmware, open drivers. Two board variants are
supported in lockstep — `rpbridge_rp2354b` (Rev-B custom PCB,
QFN-80, 48 GPIO) and `rpbridge_pico2` (off-the-shelf Raspberry Pi
Pico 2, QFN-60, 30 GPIO). Both expose the same eleven RPBP
subsystems to a Linux host as standard subsystem devices: I²C, SPI,
UART (RS232/RS485/TTL), GPIO, PWM, ADC, CAN 2.0B, WS2812 LED
strips, DMX-512, 1-Wire and I²S. Use `i2c-tools`, `libgpiod`,
`can-utils`, `spidev_test`, IIO and friends unmodified.

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
| `firmware/`            | RP2350-family firmware (C + pico-sdk 2.2.0, can2040 for CAN). Multi-board build via `RPBRIDGE_BOARD=rpbridge_rp2354b\|rpbridge_pico2`. |
| `firmware/COVERAGE.md` | Live, consumer-facing status matrix — start here to see what is shipped vs. open. |
| `driver/`              | Rust-for-Linux kernel module, DKMS-packaged.                |
| `userspace/`           | Rust workspace: protocol lib, client lib, `rpbridge-ctl`, bench. |
| `docs/`                | This documentation site (mkdocs-material).                  |
| `dts/overlays/`        | Device-tree overlays for auto-provisioning onboard sensors. |
| `hardware/`            | KiCad 9 design files (Rev-B, 2-layer, 80 × 55 mm).          |
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

| Component                  | Maturity              |
|----------------------------|-----------------------|
| Hardware — Rev-B PCB       | Draft (bring-up done) |
| Hardware — Pico 2 carrier  | Working               |
| Firmware (both boards)     | Feature complete (alpha) |
| Linux driver (Rust)        | Wire layer feature complete; subsystem adapters in progress |
| Userspace CLI (Rust)       | Working               |
| Documentation              | Synced with firmware  |
| CI/CD                      | Both boards green     |

The single source of truth for shipped functionality is
[`firmware/COVERAGE.md`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/firmware/COVERAGE.md).
For per-pin capability — including which silicon mux slots reach
which subsystem on each board — see the
[pin capability matrix](hardware/pin-matrix.md). The
[CHANGELOG](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/CHANGELOG.md)
tracks the running release log; [decision records](adr/index.md)
explain why the project shaped up the way it did.
