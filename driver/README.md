# RPBridge — Linux kernel driver

**Rust-for-Linux, out-of-tree, DKMS-packaged.** One module (`rpbridge.ko`)
matches the RPBridge composite USB device, parses its CBOR capability
map at bind time, and spawns one `auxiliary_device` per advertised
subsystem. Each auxiliary child then registers into the standard Linux
subsystem core:

| Auxiliary child      | Registers into                   | Userspace interface     |
|----------------------|----------------------------------|-------------------------|
| `rpbridge-gpio`      | `gpiolib` + cdev v2 irq_chip     | `/dev/gpiochipN`        |
| `rpbridge-i2c`       | `i2c-core` → `i2c-dev`           | `/dev/i2c-N`            |
| `rpbridge-spi`       | `spi-core` → `spidev`            | `/dev/spidevN.M`        |
| `rpbridge-pwm`       | `pwm` sysfs                      | `/sys/class/pwm/…`      |
| `rpbridge-iio`       | `iio` / `iio_buffer`             | `/sys/bus/iio/devices/…` |

The UART side (3× CDC-ACM) is handled by mainline `usbser`. The CAN side
is handled by mainline `gs_usb`. Neither lives in this module — both
drivers already exist upstream and work out of the box with the
RPBridge firmware's descriptors.

## Kernel baseline

* **Minimum**: Linux **6.13** (CONFIG_RUST=y, auxiliary_bus, Rust USB
  bindings sufficient for this module's needs).
* **Primary target**: whatever is shipping with the current Ubuntu LTS
  and Fedora. See [`kernel-baseline/SUPPORT.md`](kernel-baseline/SUPPORT.md)
  for the running support matrix.

## Build & install

### From source, against your running kernel

```sh
make                                 # preflight-checks CONFIG_RUST
sudo make modules_install            # installs into /lib/modules/$(uname -r)/extra
sudo depmod -a
sudo modprobe rpbridge
```

### Via DKMS (recommended)

```sh
sudo dkms add    driver
sudo dkms build  rpbridge/0.1.0
sudo dkms install rpbridge/0.1.0
sudo modprobe rpbridge
```

### Via distribution package

See the `.deb` / `.rpm` built by the CI workflow in
[`.github/workflows/`](../.github/workflows/).

## Tree layout

```
driver/
├── Kbuild                   # module target declaration
├── Makefile                 # out-of-tree build wrapper + preflight
├── Kconfig                  # for eventual upstream submission
├── rust-toolchain.toml      # lint/IDE toolchain (kernel uses its own)
├── dkms.conf.in             # DKMS recipe template
├── src/
│   ├── lib.rs               # crate root — module_usb_driver! + modules
│   ├── rpbp.rs              # RPBP v1 framing + CRC-32C
│   ├── cap.rs               # CBOR capability parser
│   ├── core_driver.rs       # USB probe/disconnect + auxiliary spawn
│   ├── gpio.rs              # gpio_chip child (stub)
│   ├── i2c.rs               # i2c_adapter child (stub)
│   ├── spi.rs               # spi_controller child (stub)
│   ├── pwm.rs               # pwm_chip child (stub)
│   └── iio.rs               # iio_dev child (stub)
├── udev/
│   └── 70-rpbridge.rules    # user-accessible permissions + symlinks
├── modprobe/
│   └── rpbridge.conf        # module option defaults + softdep on gs_usb
├── packaging/
│   ├── debian/              # debhelper recipe (DKMS source package)
│   └── rpm/                 # RPM spec (DKMS source package)
├── kernel-baseline/
│   └── SUPPORT.md           # tested kernel matrix
└── tests/
    └── umockdev/            # USB recordings for offline test replay
```

## Testing

```sh
# Host-runnable unit tests (CRC, framing — no kernel required)
cargo test --manifest-path ../userspace/rpbridge-protocol/Cargo.toml

# KUnit against a locally-built Rust-enabled kernel
./tools/scripts/run-kunit.sh

# umockdev replay of recorded USB sessions
./tools/scripts/run-umockdev.sh
```

## Why Rust

Historical note — the original design draft proposed a C primary +
Rust future migration. The current baseline is **Rust only**: the
Rust-for-Linux bindings required by this module (USB, auxiliary_bus,
gpio, i2c, spi, pwm, iio) are stable as of Linux 6.13, and maintaining
two parallel implementations would be pure tax. See
[`docs/adr/0003-language-choice.md`](../docs/adr/0003-language-choice.md).
