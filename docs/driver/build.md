# Driver — build & install

The kernel driver is **plain C** and uses Linux's stable in-kernel
ABI. No `CONFIG_RUST` requirement, no Rust toolchain at runtime. Any
recent distribution kernel that ships `auxiliary_bus` works.

## Prerequisites

- Linux **5.15.30+** at minimum (the `auxiliary_bus` baseline plus a
  PROBE-DEFER fix that landed mid-5.15 LTS series). Primary CI target
  is **Linux 7.0 LTS** (Ubuntu 26.04 LTS HWE). Full distribution /
  kernel matrix in
  [`driver/kernel-baseline/SUPPORT.md`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/kernel-baseline/SUPPORT.md).
- Matching kernel headers + build essentials:

  | Distribution     | Install command                                         |
  | ---------------- | ------------------------------------------------------- |
  | Debian / Ubuntu  | `sudo apt install linux-headers-$(uname -r) build-essential dkms` |
  | Fedora / Rocky   | `sudo dnf install kernel-devel kernel-headers gcc make dkms`      |
  | Arch / Manjaro   | `sudo pacman -S --needed linux-headers base-devel dkms`           |

- Required kconfig (every recent distribution kernel has these on by
  default — only check if you build your own kernel):

  ```text
  CONFIG_USB=y
  CONFIG_AUXILIARY_BUS=y
  CONFIG_GPIOLIB=y
  CONFIG_I2C=y
  CONFIG_SPI=y
  CONFIG_PWM=y
  CONFIG_IIO=y
  ```

  Quick check on the running kernel:

  ```sh
  grep -E '^CONFIG_(USB|AUXILIARY_BUS|GPIOLIB|I2C|SPI|PWM|IIO)=' \
      /boot/config-$(uname -r)
  ```

  All seven must read `=y` (or `=m` for the subsystems you're willing
  to load on demand).

## Build paths

There are three build paths depending on what you want:

### A. From source against the running kernel (developer iteration)

```sh
cd driver
make                                  # builds rpbridge.ko
sudo make modules_install             # → /lib/modules/$(uname -r)/extra/rpbridge.ko
sudo depmod -a                        # regenerate module dependency cache
sudo cp udev/70-rpbridge.rules \
        /etc/udev/rules.d/            # device-node permissions + symlinks
sudo udevadm control --reload         # tell udev to pick up the new rule
sudo modprobe rpbridge                # load it
```

Plug the device in (or `udevadm trigger` if it's already attached)
and check `dmesg | tail`:

```text
[  ...] usb 1-1: new full-speed USB device number ... using xhci_hcd
[  ...] usb 1-1: New USB device found, idVendor=0545, idProduct=0101
[  ...] rpbridge 1-1:1.4: probing matched USB interface
[  ...] rpbridge 1-1:1.4: capability map: <subsystems>
[  ...] rpbridge_gpio rpbridge.gpio.0: registered
…
```

### B. DKMS (recommended for keep-it-alive across kernel upgrades)

DKMS rebuilds the module automatically every time your distribution
ships a new kernel. One-time setup:

```sh
sudo dkms add     driver/                         # registers /usr/src/rpbridge-0.1.0/
sudo dkms build   rpbridge/0.1.0
sudo dkms install rpbridge/0.1.0                   # → /lib/modules/.../updates/dkms/
sudo cp driver/udev/70-rpbridge.rules \
        /etc/udev/rules.d/
sudo udevadm control --reload
sudo modprobe rpbridge

dkms status                                        # verify "installed"
```

After the next `apt upgrade` or `dnf upgrade` that bumps the kernel,
DKMS rebuilds the module against the new headers automatically — no
extra step required. To re-trigger explicitly:

```sh
sudo dkms autoinstall
```

### C. Distribution package (.deb / .rpm)

The CI pipeline builds both `rpbridge-dkms_<version>_all.deb` and
`rpbridge-dkms-<version>-1.noarch.rpm` on every tagged release.
Install on the target:

```sh
sudo apt install ./rpbridge-dkms_0.1.0_all.deb     # Debian / Ubuntu
sudo dnf install rpbridge-dkms-0.1.0-1.noarch.rpm  # Fedora / Rocky
```

Both packages depend on `dkms`, register the source under
`/usr/src/rpbridge-<version>/`, drop the udev rule into
`/etc/udev/rules.d/`, and run `dkms autoinstall` in the post-install
hook. Module-loading is automatic on the next `udev` event for a
matching VID/PID.

## udev rule semantics

The shipped rule
([`driver/udev/70-rpbridge.rules`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/udev/70-rpbridge.rules))
does three things:

1. Sets `MODE="0660"` and `GROUP="plugdev"` on the auxiliary
   character devices so a non-root user in `plugdev` can access
   `/dev/i2c-N`, `/dev/spidevN.M`, etc. without `sudo`.
2. Adds a stable `SYMLINK` (e.g. `/dev/rpbridge-<serial>`) so
   userspace tooling can find a specific device by serial across
   reboots.
3. Tags the device with `ID_RPBRIDGE=1` for downstream policy
   (e.g. systemd unit `BindsTo=` rules).

If you skip the udev step, the device still works — but you'll need
`sudo` to talk to its character devices, and the `/dev/rpbridge-*`
symlinks won't appear.

## Loading options

Module parameters live in
[`driver/modprobe/rpbridge.conf`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/modprobe/rpbridge.conf):

| Parameter    | Default | Meaning                                          |
| ------------ | ------- | ------------------------------------------------ |
| `debug_dump` | `0`     | When `1`, dumps every received RPBP frame to dmesg. Production should leave this off — it's chatty under load. |

Load with a custom value:

```sh
sudo modprobe rpbridge debug_dump=1
```

## Uninstall

```sh
sudo modprobe -r rpbridge
sudo dkms remove rpbridge/0.1.0 --all
sudo apt purge rpbridge-dkms             # or: sudo dnf remove rpbridge-dkms
sudo rm /etc/udev/rules.d/70-rpbridge.rules
sudo udevadm control --reload
```

## Troubleshooting

**`modprobe: ERROR: could not insert 'rpbridge': Exec format error`**
— headers don't match the running kernel. Verify with
`uname -r` vs `ls /lib/modules/`; rebuild the module against the
right kernel (`sudo dkms autoinstall` or `make clean && make`).

**`/dev/rpbridge-*` symlink missing** — udev rule isn't loaded. Check
`udevadm info /dev/i2c-N` to see whether the device matched the rule;
if not, `sudo udevadm control --reload && sudo udevadm trigger`.

**Permission denied on `/dev/i2c-N`** — current user not in
`plugdev` group. `sudo usermod -aG plugdev $USER`, then log out/in.
