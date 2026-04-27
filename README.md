# RPBridge — USB Multi-Bus Bridge for RP2350

> ## Alpha — pre-production
>
> **Status:** alpha. Do not deploy on production hosts.
>
> This project is in **alpha**. The firmware is feature-complete and
> CI-green on both supported boards (see
> [`firmware/COVERAGE.md`](firmware/COVERAGE.md) for the
> evidence-linked status matrix). The production USB VID/PID
> (`0545:0101` — BAUER GROUP / RPBridge, allocated 2026-04-27) is
> wired into the firmware, the C kernel driver, the userspace
> defaults, and the udev rules. The Linux kernel driver is plain
> C against the stable kernel-module ABI (see
> [ADR-0003 #Amendment](docs/adr/0003-language-choice.md#amendment--2026-04-26--driver-reverted-to-c)
> for why we pivoted away from Rust-for-Linux). Two pre-production
> gates remain open — they are tracked in
> [`firmware/COVERAGE.md` #Pre-production](firmware/COVERAGE.md#pre-production-gates).
> Expect APIs and the wire format to be additively-stable from
> here, but treat anything in this repo as alpha until those gates
> close.

> **Open-hardware USB-to-multi-bus bridge firmware + Linux driver
> stack for Raspberry Pi RP2350-family silicon.** Exposes I²C, SPI,
> UART (RS232/RS485/TTL), GPIO, PWM, ADC, CAN 2.0B, addressable
> LED strips, DMX-512, 1-Wire and I²S to a Linux host as if they
> were native SoC peripherals — usable via `/dev/i2c-N`,
> `/dev/spidevN.M`, `/dev/gpiochipN`, `/dev/ttyACM*`, `can0`,
> `/sys/bus/iio/devices/…`, and the matching subsystem interfaces,
> with stock userland tools (`i2cdetect`, `libgpiod`, `can-utils`,
> `spidev_test`, `iio_info`, `ola`, `w1-gpio`).
>
> Two hardware targets are supported out of the box (ADR-0008):
>
> - **`rpbridge_rp2354b`** — the BAUER GROUP custom rev-B PCB
>   (RP2354B, QFN-80, 48 GPIOs). Full feature set: 4× CDC-ACM with
>   RS232 / RS485 / TTL phys, 2× I²C, 2× SPI, 11 user GPIOs, 6 PWM,
>   4 ADC + temp, CAN, WS2812, DMX-512, 1-Wire, I²S.
> - **`rpbridge_pico2`** — bare Raspberry Pi Pico 2 (RP2350A,
>   QFN-60, 30 GPIOs, ≈ $5). Subset feature set: 1× I²C, 1× SPI,
>   8 user GPIOs, 6 PWM, 4 ADC + temp, CAN, single TTL UART,
>   WS2812, DMX-512, 1-Wire, I²S. RS232/RS485 require an external
>   transceiver wired by the user.
>
> Build: `cmake -S firmware -B build -DPICO_BOARD=rpbridge_{rp2354b|pico2}`

---

## Why this project exists

Linux developers regularly need access to low-speed peripheral buses
on machines (laptops, workstations, SBCs) that do not expose them
natively. The alternatives on the market are either closed-source
vendor dongles with bespoke APIs, or minimal USB-UART bridges that
only cover a single bus.

RPBridge unifies **eleven peripheral classes on one open-source
device** and exposes each one through the standard Linux subsystem,
so every existing tool, script and service keeps working unmodified.
There is no proprietary host SDK to learn, no per-vendor daemon to
run, and no `/dev/rpbridgeX` private device — every bus shows up
under the canonical kernel interface for that bus.

## Feature matrix

| Bus / Peripheral | Host-side interface              | Standard tools that work unchanged                          |
|------------------|----------------------------------|-------------------------------------------------------------|
| I²C master       | `/dev/i2c-N`                     | `i2c-tools`, `libi2c`, `smbus2`, kernel client drivers      |
| SPI master       | `/dev/spidevN.M`                 | `spidev_test`, `flashrom`, `libmpsse`                       |
| UART × up to 4   | `/dev/ttyACM*`                   | `minicom`, `picocom`, `pyserial`, `tio`                     |
| GPIO + IRQ       | `/dev/gpiochipN` (cdev v2)       | `libgpiod 2.x`, `gpioset`, `gpiomon`                        |
| PWM              | `/sys/class/pwm/pwmchipN`        | sysfs PWM, kernel pwm-client drivers                        |
| ADC (IIO)        | `/sys/bus/iio/devices/…`         | `iio_info`, `iio_readdev`, `libiio`, Grafana                |
| CAN 2.0B         | `can0` (SocketCAN)               | `can-utils`, `python-can`, `SavvyCAN`                       |
| WS2812 / SK6812  | RPBP `LED` subsystem (vendor IF) | `rpbridge-ctl led`, `python-rpbridge`                       |
| DMX-512 (TX)     | RPBP `DMX` subsystem             | `ola_dev_info`, `rpbridge-ctl dmx`                          |
| 1-Wire           | `/sys/bus/w1/devices/…`          | `w1-gpio` compatible (`w1_therm`, etc.)                     |
| I²S TX           | RPBP `I²S` subsystem             | `rpbridge-ctl i2s`, `python-rpbridge`                       |

OTA firmware update runs through the RP2350 ROM-BOOTSEL / UF2
mechanism — no custom bootloader, no signing ceremony to start
using the device. Production releases will gain a `minisign`
signature on the artefact (see [SECURITY.md](SECURITY.md)).

For the protocol contract, see
[`docs/protocol/rpbp-v1.md`](docs/protocol/rpbp-v1.md). For the
exact pin assignments per board, see
[`docs/hardware/pin-matrix.md`](docs/hardware/pin-matrix.md).

## Project status

This README intentionally separates the per-component status from
the protocol-level status. **Source of truth:**
[`firmware/COVERAGE.md`](firmware/COVERAGE.md) for the firmware,
[`driver/TODO.md`](driver/TODO.md) for the Rust driver.

- **RPBP wire format** — frozen at v1; additive-only changes from
  here (MAJOR bump required for breaking change).
- **Firmware (rev-B)** — feature-complete: 11 subsystems, on-device
  SELFTEST, host-side roundtrip CI.
- **Firmware (Pi Pico 2)** — feature-complete subset variant;
  per-variant CI matrix green.
- **Linux driver** — C, 9 subsystem children via `auxiliary_bus`.
  Compiles 0/0 against Linux 7.0 (Ubuntu 26.04 LTS HWE). See
  ADR-0003.
- **CAN via gs_usb** — works via mainline kernel `gs_usb.ko`; no
  out-of-tree driver needed.
- **Userspace `rpbridge-ctl`** — scaffolding present; per-subsystem
  wire layers complete (46 tests).
- **Documentation** — cross-linked: protocol spec, ADRs,
  per-subsystem wire docs, pin matrix, coverage matrix.
- **Hardware (rev-B)** — schematic + placement done; production
  VID/PID `0545:0101` allocated and wired.
- **Pre-production gates** — 2 open: `bcdDevice` bump, umockdev
  fixture against real-hw traffic. See
  [COVERAGE](firmware/COVERAGE.md#pre-production-gates).

Architecture decisions are tracked under
[`docs/adr/`](docs/adr/) — a living list explaining *why* the
codebase looks the way it does. Notable recent ones:

- [ADR-0005](docs/adr/0005-driver-optional-coexistence.md) — single
  USB configuration; CDCs always on, driver strictly additive.
- [ADR-0008](docs/adr/0008-multi-board-variants.md) — two board
  headers + feature flags; one source tree, multiple targets.
- [ADR-0009](docs/adr/0009-pin-function-flexibility.md) — protocol
  surface for runtime pin-function discovery and the planned
  high-speed counter subsystem.

## Repository layout

```text
.
├── firmware/                  # RP2350 firmware (C, pico-sdk 2.2.0)
│   ├── src/
│   │   ├── rpbp/              #   RPBP v1 framing, dispatcher, stream layer
│   │   ├── capabilities/      #   CBOR capability-map emitter
│   │   ├── commands/          #   per-subsystem opcode handlers
│   │   ├── peripherals/       #   pin broker + per-bus drivers
│   │   ├── usb/               #   composite descriptor set, gs_usb glue, CDC pump
│   │   ├── io_control/        #   second-core scheduler + IPC queues
│   │   └── pio/               #   .pio assembly programs
│   ├── boards/                # PICO_BOARD headers (rpbridge_rp2354b, rpbridge_pico2)
│   ├── tests/host/            # host-side wire-format unit tests + golden corpus
│   ├── third_party/           # pico-sdk 2.2.0 + can2040 (pinned submodules)
│   ├── COVERAGE.md            # at-a-glance "is firmware done & tested?" matrix
│   ├── TODO.md                # developer working backlog (granular)
│   └── CMakeLists.txt
│
├── driver/                    # Linux kernel module (plain C, ≥ 5.15 LTS)
│   ├── src/                   #   wire layer + per-subsystem clients + aux children
│   ├── tests/                 #   KUnit + umockdev harnesses
│   ├── packaging/             #   debian + rpm DKMS spec files
│   ├── kernel-baseline/       #   tested kernel versions + kconfig requirements
│   ├── udev/                  #   /lib/udev/rules.d/70-rpbridge.rules
│   └── modprobe/              #   /etc/modprobe.d snippets
│
├── userspace/                 # Rust workspace: protocol lib + CLI + bench tool
│   ├── rpbridge-protocol/     #   no_std-capable RPBP v1 framing library
│   ├── rpbridge/              #   high-level client library
│   ├── rpbridge-ctl/          #   CLI: info, flash, selftest, gpio, i2c, …
│   └── rpbridge-bench/        #   benchmark harness
│
├── docs/                      # mkdocs-material documentation site
│   ├── architecture/          #   system architecture
│   ├── protocol/              #   RPBP v1 specification + per-subsystem wire docs
│   │   └── subsystems/        #   sys, i2c, spi, gpio, pwm, adc, uart, led, dmx, onewire, i2s
│   ├── firmware/              #   firmware developer guide + resource budget
│   ├── driver/                #   Linux driver guide
│   ├── hardware/              #   board design + pinout + pin capability matrix
│   ├── adr/                   #   Architecture Decision Records (0001..0009)
│   └── drafts/                #   original design drafts (preserved as historical)
│
├── dts/overlays/              # device-tree overlays for aux-provisioned clients
├── hardware/                  # KiCad 9 design files (rev-B, 2-layer, 80×55 mm)
├── tools/                     # dev scripts: build, flash, setup-env (POSIX + PowerShell)
└── tests/                     # integration fixtures, golden RPBP vectors
```

## Getting started — three minutes to hello-world

```sh
# 1. clone with pinned submodules
git clone --recurse-submodules https://github.com/<org>/USB-Multi-Bus-Bridge.git
cd USB-Multi-Bus-Bridge

# 2. install host toolchains
./tools/setup-dev-env.sh            # gcc-arm-none-eabi, cmake, rustup, dkms, picotool

# 3. build the firmware (default: rev-B)
./tools/build.sh firmware           # firmware/build/rev-b/rpbridge.uf2
# or for Pi Pico 2:
./tools/build.sh firmware --board rpbridge_pico2

# 4. build the Linux driver (gated on R4L kernel ≥ 6.13)
./tools/build.sh driver             # driver/*.ko via DKMS

# 5. build the userspace Rust tools
./tools/build.sh userspace          # userspace/target/release/rpbridge-ctl

# 6. flash + load
./tools/flash.sh firmware/build/rev-b/rpbridge.uf2
sudo insmod driver/rpbridge.ko      # once R4L adapters are in
dmesg | tail && lsusb | grep -i rpbridge
```

Once flashed and plugged in:

```sh
$ ls /dev/i2c-* /dev/spidev* /dev/gpiochip* /dev/ttyACM*
$ ip link show can0
$ iio_info | grep rpbridge
$ rpbridge-ctl selftest --json
```

## Design principles

The design follows four leading principles, detailed in
[`docs/architecture/`](docs/architecture/) and the
[design drafts](docs/drafts/):

1. **Self-describing device.** Host parses a CBOR capability map at
   bind time; no firmware-version matrix to maintain. The map
   carries an `access` field per subsystem (ADR-0005) so hosts know
   whether to talk through the CDC or the vendor pipe without
   guessing.
2. **Standard Linux subsystems.** No private `/dev/rpbridgeX` for
   bus access — the device appears under the canonical subsystem
   interfaces. UART deliberately stays on inbox `usbser` (CDC-ACM)
   so a stock terminal works without any driver at all.
3. **Modern stack only.** `auxiliary_bus` (≥ 5.11), GPIO cdev v2
   (≥ 5.10), libgpiod 2.x, IIO, SocketCAN. Deprecated paths
   (`/sys/class/gpio`, `platform_device`-MFD) are intentionally not
   implemented.
4. **Forward-/backward-compatible.** Semantic versioning on four
   independent axes (protocol, firmware, driver, userspace). New
   features land via additive feature flags + capability-map keys,
   not version gates.

## Licensing

- Firmware: **BSD-3-Clause** (pico-sdk-compatible).
- Linux driver: **GPL-2.0** (kernel requirement).
- Userspace Rust crates & CLI: **Apache-2.0 OR MIT** (Rust-ecosystem default).
- Documentation: **CC-BY-4.0**.

SPDX identifiers are set on every source file. See
[`LICENSE`](LICENSE) for the primary license text and
[`docs/licensing.md`](docs/licensing.md) for the full mapping.

## Contributing

Please read [`CONTRIBUTING.md`](CONTRIBUTING.md) first. All commits
must be signed-off (`-s`), all PRs must pass the pre-merge CI
(checkpatch, sparse, `cargo clippy -- -D warnings`, `cargo test`,
firmware build matrix on both board variants). The full commit-style
guide is in [`docs/commit-style.md`](docs/commit-style.md).

## Security

Firmware release artifacts are signed with `minisign` (Ed25519).
Public key and verification instructions live in
[`SECURITY.md`](SECURITY.md) and
[`docs/releases.md`](docs/releases.md).

---

*BAUER GROUP — Today, Tomorrow, Together.*
