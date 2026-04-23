# RPBridge — USB Multi-Bus Bridge on RP2354B

> **Professional-grade USB-to-multi-bus bridge firmware + Linux driver stack
> for the Raspberry Pi RP2354B.** Exposes I²C, SPI, UART (RS232/RS485/TTL),
> GPIO, PWM, ADC and CAN 2.0B to a Linux host as if they were native SoC
> peripherals — usable via `/dev/i2c-N`, `/dev/spidevN.M`, `/dev/gpiochipN`,
> `/dev/ttyACM*`, `can0`, and `/sys/bus/iio/devices/…`, with stock userland
> tools (`i2cdetect`, `libgpiod`, `can-utils`, `spidev_test`, `iio_info`).

---

## Why this project exists

Linux developers regularly need access to low-speed peripheral buses on
machines (laptops, workstations, SBCs) that do not expose them natively. The
alternatives on the market are either closed-source vendor dongles with
bespoke APIs, or minimal USB-UART bridges that only cover a single bus.

RPBridge unifies **seven bus classes on one open-source device** and exposes
each one through the standard Linux subsystem, so every existing tool, script
and service keeps working unmodified.

## Feature matrix

| Bus / Peripheral | Host-side interface          | Standard tools that work unchanged            |
|------------------|------------------------------|-----------------------------------------------|
| I²C master       | `/dev/i2c-N`                 | `i2c-tools`, `libi2c`, `smbus2`, kernel client drivers |
| SPI master       | `/dev/spidevN.M`             | `spidev_test`, `flashrom`, `libmpsse`         |
| UART ×3          | `/dev/ttyACM*`               | `minicom`, `picocom`, `pyserial`, `tio`       |
| GPIO + IRQ       | `/dev/gpiochipN` (cdev v2)   | `libgpiod 2.x`, `gpioset`, `gpiomon`          |
| PWM              | `/sys/class/pwm/pwmchipN`    | sysfs PWM, kernel pwm-client drivers          |
| ADC (IIO)        | `/sys/bus/iio/devices/…`     | `iio_info`, `iio_readdev`, `libiio`, Grafana  |
| CAN 2.0B         | `can0` (SocketCAN)           | `can-utils`, `python-can`, `SavvyCAN`         |
| 1-Wire (optional)| `/sys/bus/w1/devices/…`      | `w1-gpio` compatible                          |

OTA firmware update runs through the RP2350 ROM-BOOTSEL / UF2 mechanism —
no custom bootloader, no signing ceremony to start using the device.

## Repository layout

```
.
├── firmware/           # RP2354B firmware (C, pico-sdk)
│   ├── src/            # application sources
│   ├── boards/         # custom PICO_BOARD header: rpbridge_rp2354b
│   ├── third_party/    # pico-sdk 2.2.0 + can2040 as pinned git submodules
│   └── CMakeLists.txt
│
├── driver/             # Linux kernel module (Rust-for-Linux, ≥ 6.13)
│   ├── src/            # Rust sources: core, cap-parser, per-subsystem children
│   ├── tests/          # KUnit + umockdev harnesses
│   ├── packaging/      # debian + rpm DKMS spec files
│   ├── kernel-baseline/# tested kernel versions + kconfig requirements
│   ├── udev/           # /lib/udev/rules.d/70-rpbridge.rules
│   └── modprobe/       # /etc/modprobe.d snippets
│
├── userspace/          # Rust workspace: protocol lib + CLI + bench tool
│   ├── rpbridge-protocol/  # no_std-capable RPBP v1 framing library
│   ├── rpbridge/           # high-level client library
│   ├── rpbridge-ctl/       # CLI: info, flash, selftest, gpio, i2c, …
│   └── rpbridge-bench/     # benchmark harness
│
├── docs/               # mkdocs-material documentation site
│   ├── architecture/   # system architecture
│   ├── protocol/       # RPBP v1 specification
│   ├── firmware/       # firmware developer guide
│   ├── driver/         # Linux driver guide
│   ├── hardware/       # Rev-B hardware notes
│   ├── adr/            # Architecture Decision Records
│   └── drafts/         # original design drafts (preserved)
│
├── dts/overlays/       # device-tree overlays for aux-provisioned clients
├── hardware/           # KiCad 9 design files (rev-B, 2-layer, 80×55 mm)
├── tools/              # dev scripts: build, flash, setup-env
└── tests/              # integration fixtures, golden RPBP vectors
```

## Getting started — three minutes to hello-world

```sh
# 1. clone with pinned submodules
git clone --recurse-submodules https://github.com/<org>/USB-Multi-Bus-Bridge.git
cd USB-Multi-Bus-Bridge

# 2. install host toolchains
./tools/setup-dev-env.sh            # installs gcc-arm-none-eabi, cmake, rustup, dkms

# 3. build the firmware
./tools/build.sh firmware           # produces firmware/build/rpbridge.uf2

# 4. build the Linux driver
./tools/build.sh driver             # produces driver/linux-c/*.ko

# 5. build the userspace Rust tools
./tools/build.sh userspace          # produces userspace/target/release/rpbridge-ctl

# 6. flash + load
./tools/flash.sh firmware/build/rpbridge.uf2
sudo insmod driver/linux-c/rpbridge-core.ko
dmesg | tail && lsusb | grep -i rpbridge
```

Once flashed and plugged in you should see:

```sh
$ ls /dev/i2c-* /dev/spidev* /dev/gpiochip* /dev/ttyACM*
$ ip link show can0
$ iio_info | grep rpbridge
```

## Design principles

The design follows four leading principles, detailed in
[`docs/architecture/`](docs/architecture/) and the
[design drafts](docs/drafts/):

1. **Self-describing device.** Host parses a CBOR capability map at bind time;
   no firmware-version matrix to maintain.
2. **Standard Linux subsystems.** No private `/dev/rpbridgeX` for bus access —
   the device appears under the canonical subsystem interfaces.
3. **Modern stack only.** `auxiliary_bus` (≥5.11), GPIO cdev v2 (≥5.10),
   libgpiod 2.x, IIO, SocketCAN. Deprecated paths (`/sys/class/gpio`,
   `platform_device`-MFD) are intentionally not implemented.
4. **Forward-/backward-compatible.** Semantic versioning on four independent
   axes (protocol, firmware, driver, userspace). Feature flags, not version
   gates.

See [ADR-0001](docs/adr/0001-protocol-choice.md) for why RPBP replaces AT-style
framing, [ADR-0002](docs/adr/0002-driver-bus-choice.md) for why
`auxiliary_bus` replaces the classic MFD platform_device pattern, and
[ADR-0003](docs/adr/0003-language-choice.md) for the split between C kernel
code and Rust userspace.

## Licensing

* Firmware: **BSD-3-Clause** (pico-sdk-compatible).
* Linux driver: **GPL-2.0** (kernel requirement).
* Userspace Rust crates & CLI: **Apache-2.0 OR MIT** (Rust-ecosystem default).
* Documentation: **CC-BY-4.0**.

SPDX identifiers are set on every source file. See [`LICENSE`](LICENSE) for
the primary license text and [`docs/licensing.md`](docs/licensing.md) for
the full mapping.

## Status

| Component           | Maturity      |
|---------------------|---------------|
| Hardware (rev-B)    | Draft — see `docs/drafts/rpbridge-hardware-design.md` |
| Firmware            | Scaffold      |
| Linux driver (C)    | Scaffold      |
| Linux driver (Rust) | Parking spot  |
| Userspace CLI       | Scaffold      |
| Documentation       | In progress   |
| CI/CD               | Scaffold      |

## Contributing

Please read [`CONTRIBUTING.md`](CONTRIBUTING.md) first. All commits must be
signed-off (`-s`), all PRs must pass the pre-merge CI (checkpatch, sparse,
`cargo clippy -- -D warnings`, `cargo test`, firmware build matrix). The full
commit-style guide is in [`docs/commit-style.md`](docs/commit-style.md).

## Security

Firmware release artifacts are signed with `minisign` (Ed25519). Public key and
verification instructions live in [`SECURITY.md`](SECURITY.md) and
[`docs/releases.md`](docs/releases.md).

---

*BAUER GROUP — Today, Tomorrow, Together.*
