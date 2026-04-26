# Linux driver — overview

The RPBridge Linux driver is a **single C kernel module
(`rpbridge.ko`)** that matches the RPBridge USB composite device,
discovers its capabilities, and spawns one `auxiliary_device` per
advertised subsystem. Each child registers into the standard
Linux subsystem core.

| Aux child            | Registers into                   | Userspace interface      |
|----------------------|----------------------------------|--------------------------|
| `rpbridge-gpio`      | `gpiolib`                        | `/dev/gpiochipN`         |
| `rpbridge-i2c`       | `i2c-core` → `i2c-dev`           | `/dev/i2c-N`             |
| `rpbridge-spi`       | `spi-core` → `spidev`            | `/dev/spidevN.M`         |
| `rpbridge-pwm`       | `pwm` sysfs                      | `/sys/class/pwm/pwmchipN`|
| `rpbridge-iio`       | `iio` (single-shot)              | `/sys/bus/iio/devices/…` |
| `rpbridge-led`       | misc cdev                        | `/dev/rpbridge-led<N>`   |
| `rpbridge-dmx`       | misc cdev                        | `/dev/rpbridge-dmx<N>`   |
| `rpbridge-onewire`   | `w1_bus_master`                  | `/sys/bus/w1/`           |
| `rpbridge-i2s`       | misc cdev                        | `/dev/rpbridge-i2s<N>`   |

The UART side (3× CDC-ACM) is handled by mainline `cdc_acm`. The
CAN side is handled by mainline `drivers/net/can/usb/gs_usb.ko`.
Neither lives in this module.

## Why C

The driver was first attempted in Rust-for-Linux. R4L's API is
still stabilising — between Linux 6.18 and 7.0 the
`kernel::usb::Driver` trait was restructured, `KString` removed,
`#[vtable]` mechanics dropped, and the entire probe lifecycle
moved to a `PinInit`-style construction model. Out-of-tree R4L
modules pay this drift cost on every major kernel release for as
long as the API surface stays unstable.

For a driver that needs to land on **whatever Linux Ubuntu ships
today** — currently 7.0, 8.x within a year — Linux's hard ABI
stability guarantee for C makes the maintenance equation flip.
A C driver compiles unchanged across years of kernel releases.

The earlier Rust scaffold remains on the `archive/rust-driver`
git branch for reference. See
[`adr/0003-language-choice.md`](../adr/0003-language-choice.md)
§Amendment for the full reasoning.

## Kernel baseline

- **Minimum**: Linux **5.15 LTS** (auxiliary_bus baseline).
- **Primary CI target**: Linux **7.0 LTS** (Ubuntu 26.04 LTS HWE
  default).
- **No `CONFIG_RUST` requirement.** The driver compiles via plain
  kbuild against any modern kernel-headers package.

See [`driver/kernel-baseline/SUPPORT.md`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/kernel-baseline/SUPPORT.md)
for the full distribution / kernel-version compatibility matrix.

## Lifecycle

```text
  usb_driver.probe()
       │
       ├─ kzalloc per-device state, kref_init
       ├─ usb_get_dev() the parent USB device
       ├─ find bulk IN/OUT endpoints
       ├─ usb_set_intfdata() so disconnect can find us
       ├─ HELLO  (version gate; -EPROTONOSUPPORT on mismatch)
       ├─ GET_CAPABILITIES, parse CBOR map
       ├─ for each advertised supported class
       │     auxiliary_device_register(rpbridge-<class>.<idx>)
       └─ return 0

  usb_driver.disconnect()
       │
       ├─ usb_set_intfdata(NULL)        // block latent callbacks
       ├─ unregister every aux child    // each kref_put's parent
       └─ rpbridge_dev_put()            // last drop frees state
```

Refcounting: every aux child takes an extra reference on the
parent state via `rpbridge_dev_get()` in `spawn_one()`, dropped
in the aux device's release callback. The kref guarantees that a
late callback firing after `disconnect()` returns still finds
valid state, and that the final teardown happens exactly once.

## Packaging

### DKMS

```sh
sudo dkms add driver
sudo dkms build rpbridge/0.1.0
sudo dkms install rpbridge/0.1.0
sudo modprobe rpbridge
```

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
(`/dev/rpbridge-uart1`, `/dev/rpbridge-debug`,
`/dev/rpbridge-bootsel`).

## Testing

Test ladders that run (or will run) in CI:

1. **Compile gate** — `tools/build.sh driver` against the Linux
   7.0 LTS kernel-headers shipped with Ubuntu 26.04 LTS in the
   builder Docker image. 0 warnings, 0 errors enforced.
2. **checkpatch** — `make checkpatch` runs the kernel tree's own
   `scripts/checkpatch.pl --strict` against `driver/src/`. Local
   today, CI gate planned.
3. **umockdev replay** — recorded USB sessions exercise probe /
   disconnect paths without real hardware. Fixture capture is
   pending on first hardware availability (see
   `driver/TODO.md` P1).
4. **Hardware end-to-end** — `tests/e2e.md` 10-section checklist
   to be executed against rev-B for sign-off.

See [`driver/README.md`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/README.md)
for the source-tree layout and
[`driver/TODO.md`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/TODO.md)
for the prioritised open-work list.
