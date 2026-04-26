# Kernel support matrix

| Kernel version | Status | Notes |
| --- | --- | --- |
| 5.15 LTS | **minimum supported** | First kernel with stable `auxiliary_bus`. The driver requires nothing beyond standard subsystem APIs available since 5.15 (USB, GPIO, I²C, SPI, PWM, IIO, auxiliary_bus). |
| 6.x | supported | Continuously verified by CI against the headers Ubuntu 26.04 LTS ships. |
| **7.0 LTS (Ubuntu 26.04 HWE default)** | **primary CI target** | The build container's `linux-headers-generic` + `linux-lib-rust-7.0.0-14-generic` pair. Every PR in the repo's GitHub Actions workflow re-compiles the module against this kernel. |
| 8.x and beyond | expected OK | Linux's stable kernel-module ABI guarantee carries forward; we do not pre-emptively gate against future versions. CI will surface any in-tree breaks. |

## Required kconfig

The target kernel must have:

```
CONFIG_USB=y
CONFIG_AUXILIARY_BUS=y
CONFIG_GPIOLIB=y
CONFIG_I2C=y
CONFIG_SPI=y
CONFIG_PWM=y
CONFIG_IIO=y
```

These are baseline distro defaults on Ubuntu / Debian / Fedora / Arch.
The driver is **C-only** since the v0.1.0 rewrite; no `CONFIG_RUST`
dependency. Earlier `Rust-for-Linux` snapshots of the driver are
preserved on the `archive/rust-driver` git branch for reference.

## Distributions shipping compatible kernels

| Distribution | Kernel | Notes |
| --- | --- | --- |
| Ubuntu 26.04 LTS | 7.0 (HWE default) | **Primary supported runtime.** DKMS package builds clean against the shipped headers. |
| Ubuntu 24.04 LTS | 6.8 / 6.11 (HWE) | Compiles cleanly; not part of the CI matrix but expected to work via auxiliary_bus baseline. |
| Debian 13 Trixie | 6.12 | Compiles cleanly. |
| Fedora 42+ | 6.18+ | Compiles cleanly. |
| Arch Linux | rolling | Compiles cleanly against current mainline. |

## Distributions explicitly NOT supported

| Distribution | Kernel | Reason |
| --- | --- | --- |
| Ubuntu 22.04 LTS | 5.15 (default) | 5.15.0 had a regression in `auxiliary_bus` PROBE-DEFER handling that wasn't backported until 5.15.30. The minimum-stable 5.15.30+ HWE kernel works; `linux-image-generic` baseline doesn't. |
| RHEL 9.0–9.2 | 5.14 | No `auxiliary_bus`. |
| Anything older | < 5.15 | No `auxiliary_bus`. |

## How to check the target kernel quickly

```sh
grep -E '^CONFIG_(USB|AUXILIARY_BUS|GPIOLIB|I2C|SPI|PWM|IIO)=' \
    /boot/config-$(uname -r)
```

All seven must read `=y` (or `=m` for the subsystems you're willing
to load on demand). If any is missing, rebuild the kernel with it
enabled or switch distribution.
