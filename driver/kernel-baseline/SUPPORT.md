# Kernel support matrix

| Kernel version | Status        | Notes                                          |
|----------------|---------------|------------------------------------------------|
| 6.13 LTS       | **required**  | Minimum baseline — Rust-for-Linux USB + aux_bus stable enough for this module. |
| 6.14           | tested        | Primary CI target.                             |
| 6.15+          | expected OK   | Tracked via the `nightly-mainline` CI workflow; breakages open an automatic issue. |
| < 6.13         | unsupported   | Either lacks `CONFIG_RUST=y` in distro builds or has unstable bindings we depend on. |

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

* **Ubuntu 25.04+** (noble HWE path) — kernel 6.14 with Rust enabled.
* **Fedora 42+**.
* **Arch Linux** — rolling, current mainline.
* **Debian 13 (trixie)** once it ships 6.13 LTS in its backports channel.

Older distributions (Ubuntu 22.04, Debian 12, RHEL 9) are **not
supported targets** for this driver. Consider upgrading the kernel via
the distribution's HWE / kernel-ml channel, or run the driver on a
newer workstation.

## How to check the target kernel quickly

```sh
grep -E '^CONFIG_(RUST|AUXILIARY_BUS|USB|GPIOLIB|I2C|SPI|PWM|IIO)=' /boot/config-$(uname -r)
```

All eight must read `=y` (or `=m` for the subsystems you don't mind
loading on demand). If any is missing, rebuild the kernel with it
enabled or switch distribution.
