# Kernel support matrix

| Kernel version | Status | Notes |
| --- | --- | --- |
| 6.18 | **required** | Minimum baseline — `kernel::usb::Driver` (Daniel Almeida's Rust USB abstractions, Collabora) is available here. Earlier kernels lack the USB driver trait this module's outer shell uses, even when `CONFIG_RUST=y` is on. |
| 6.19+ | expected OK | Tracked via the `nightly-mainline` CI workflow; breakages open an automatic issue. |
| 6.13..6.17 | unsupported | Rust USB abstractions not yet upstream. The FFI layer (ADR-0007) still needs `kernel::usb::Driver` as its outer shell, so older kernels are gated out at build time. |
| < 6.13 | unsupported | Either lacks `CONFIG_RUST=y` in distro builds or has unstable bindings we depend on. |

## Required kconfig

The target kernel must be built with:

```
CONFIG_RUST=y
CONFIG_AUXILIARY_BUS=y
CONFIG_USB=y
CONFIG_GPIOLIB=y
CONFIG_I2C=y
CONFIG_SPI=y
CONFIG_PWM=y
CONFIG_IIO=y
```

## Distributions shipping compatible kernels

Kernel 6.18 was released in early 2026; distribution availability
(April 2026):

* **Arch Linux** — rolling, current mainline, shipping 6.18+.
* **Fedora 42+** — ships 6.18 via kernel updates.
* **Ubuntu 26.04 LTS** — ships with 6.18 as its HWE default.
* **Debian 13** — 6.18 via backports.

Older distributions (Ubuntu 25.04, Fedora 41, Debian 12, RHEL 9)
are **not supported targets** for this driver. Consider upgrading
the kernel via the distribution's HWE / kernel-ml channel, or run
the driver on a newer workstation.

## How to check the target kernel quickly

```sh
grep -E '^CONFIG_(RUST|AUXILIARY_BUS|USB|GPIOLIB|I2C|SPI|PWM|IIO)=' /boot/config-$(uname -r)
```

All eight must read `=y` (or `=m` for the subsystems you don't mind
loading on demand). If any is missing, rebuild the kernel with it
enabled or switch distribution.
