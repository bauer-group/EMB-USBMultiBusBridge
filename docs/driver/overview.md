# Linux driver — overview

The RPBridge Linux driver is a **single Rust-for-Linux module
(`rpbridge.ko`)** that matches the RPBridge USB composite device,
discovers its capabilities, and spawns one
`auxiliary_device` per advertised subsystem. Each child registers
into the standard Linux subsystem core.

| Aux child            | Registers into                   | Userspace interface      |
|----------------------|----------------------------------|--------------------------|
| `rpbridge-gpio`      | `gpiolib` + cdev-v2 `irq_chip`   | `/dev/gpiochipN`         |
| `rpbridge-i2c`       | `i2c-core` → `i2c-dev`           | `/dev/i2c-N`             |
| `rpbridge-spi`       | `spi-core` → `spidev`            | `/dev/spidevN.M`         |
| `rpbridge-pwm`       | `pwm` sysfs                      | `/sys/class/pwm/pwmchipN`|
| `rpbridge-iio`       | `iio` / `iio_buffer`             | `/sys/bus/iio/devices/…` |

The UART side (3× CDC-ACM) is handled by mainline `usbser`. The CAN
side is handled by mainline `drivers/net/can/usb/gs_usb.ko`.
Neither lives in this module.

## Why Rust, and why only Rust

Rust-for-Linux has stabilised the kernel abstractions this driver
needs (USB, `auxiliary_bus`, gpiolib, i2c, spi, pwm, iio) by Linux
6.13. A single implementation is easier to audit, refactor and test
than a parallel C/Rust pair, so this module is **Rust only**. See
[`adr/0003-language-choice.md`](../adr/0003-language-choice.md).

## Kernel baseline

* **Required**: Linux ≥ 6.13 with `CONFIG_RUST=y`.
* **Tested primary target**: whatever mainline Ubuntu / Fedora /
  Arch ships today.

Distributions that do not ship a kernel with `CONFIG_RUST=y` cannot
run this driver. Upgrade the kernel via HWE / kernel-ml / Fedora
Rawhide, or install on a more current distribution.

## Lifecycle

```text
  usb_driver.probe()
       │
       ├─ allocate per-device state (Arc<RpbridgeState>)
       ├─ claim bulk IN/OUT endpoints, anchor a pool of URBs
       ├─ send HELLO, receive HELLO (version gate)
       ├─ send CMD_REQUEST{GET_CAPABILITIES}, parse CBOR
       ├─ for each advertised subsystem
       │     auxiliary_device_register(rpbridge-<class>.<n>)
       └─ return Ok(state)

  usb_driver.disconnect()
       │
       ├─ usb_kill_anchored_urbs()
       ├─ auxiliary_device_unregister() for every child
       └─ drop per-device state
```

## Packaging

### DKMS

```sh
sudo dkms add driver
sudo dkms build rpbridge/0.1.0
sudo dkms install rpbridge/0.1.0
sudo modprobe rpbridge
```

The wrapper `Makefile` preflight-checks that the target kernel has
`CONFIG_RUST=y` and bails out with a clear message otherwise.

### Debian / Ubuntu

Built by the `driver-package.yml` CI workflow:

```sh
sudo apt install ./rpbridge-dkms_0.1.0_all.deb
```

### Fedora / Rocky

```sh
sudo dnf install rpbridge-dkms-0.1.0-1.noarch.rpm
```

## udev

The module package installs
`/usr/lib/udev/rules.d/70-rpbridge.rules`, which grants
`plugdev`-group write access and sets stable symlinks
(`/dev/rpbridge-uart1`, `/dev/rpbridge-debug`, `/dev/rpbridge-bootsel`).

## Testing

Three test ladders run in CI:

1. **Host unit tests** (`cargo test`) on `rpbridge-protocol` —
   framing + CRC + property tests.
2. **KUnit** against a locally-built Rust-enabled kernel — covers
   the driver's pure logic.
3. **umockdev replay** of recorded USB sessions — covers probe /
   disconnect / suspend / resume paths without real hardware.

See [`driver/README.md`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/README.md)
for the source-tree layout and the current TODO list.
