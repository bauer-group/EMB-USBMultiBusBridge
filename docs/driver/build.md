# Driver — build & install

## Prerequisites

- Linux ≥ 6.13 with `CONFIG_RUST=y`, `CONFIG_AUXILIARY_BUS=y`, and
  the subsystem cores this module uses (`GPIOLIB`, `I2C`, `SPI`,
  `PWM`, `IIO`). See
  [`kernel-baseline/SUPPORT.md`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/driver/kernel-baseline/SUPPORT.md).
- Matching kernel headers (`linux-headers-$(uname -r)` on Debian/
  Ubuntu, `kernel-devel` on Fedora/Rocky).
- `rustup` with a toolchain roughly matching the kernel's own Rust
  pin — the kernel uses its own toolchain at build time, so this is
  for IDE/rust-analyzer support only.

## From source

```sh
cd driver
make                                # preflight + build
sudo make modules_install           # /lib/modules/$(uname -r)/extra
sudo depmod -a
sudo modprobe rpbridge
```

## DKMS

```sh
sudo dkms add     driver
sudo dkms build   rpbridge/0.1.0
sudo dkms install rpbridge/0.1.0
sudo modprobe rpbridge
```

## Distro packages

Install the CI-built `.deb` or `.rpm`:

```sh
sudo apt install ./rpbridge-dkms_0.1.0_all.deb
sudo dnf install rpbridge-dkms-0.1.0-1.noarch.rpm
```

## Loading options

Tunables live in
[`driver/modprobe/rpbridge.conf`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/driver/modprobe/rpbridge.conf):

| Parameter     | Default | Meaning                                         |
|---------------|---------|-------------------------------------------------|
| `debug_dump`  | `0`     | When `1`, dumps received RPBP frames to dmesg. Keep disabled in production. |

## Uninstall

```sh
sudo modprobe -r rpbridge
sudo dkms remove rpbridge/0.1.0 --all
sudo apt purge rpbridge-dkms          # or: sudo dnf remove rpbridge-dkms
```
