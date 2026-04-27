# RPBridge — Linux kernel driver

**C, out-of-tree, DKMS-packaged.** One module (`rpbridge.ko`) matches
the RPBridge composite USB device, parses its CBOR capability map at
bind time, and spawns one `auxiliary_device` per advertised subsystem.
Each auxiliary child then registers into the standard Linux subsystem
core:

| Auxiliary child      | Registers into                   | Userspace interface     |
|----------------------|----------------------------------|-------------------------|
| `rpbridge-gpio`      | `gpiolib` + cdev v2 irq_chip     | `/dev/gpiochipN`        |
| `rpbridge-i2c`       | `i2c-core` → `i2c-dev`           | `/dev/i2c-N`            |
| `rpbridge-spi`       | `spi-core` → `spidev`            | `/dev/spidevN.M`        |
| `rpbridge-pwm`       | `pwm` sysfs                      | `/sys/class/pwm/…`      |
| `rpbridge-iio`       | `iio` / `iio_buffer`             | `/sys/bus/iio/devices/…` |

The UART side (4× CDC-ACM — three UART1 transceiver banks plus
UART2 via PIO) is handled by mainline `cdc_acm`. The CAN side is
handled by mainline `gs_usb`. Neither lives in this module — both
drivers already exist upstream and work out of the box with the
RPBridge firmware's descriptors.

## Kernel baseline

* **Minimum**: Linux **5.15 LTS** (auxiliary_bus baseline).
* **Primary CI target**: Linux **7.0 LTS** (Ubuntu 26.04 LTS HWE
  default). See [`kernel-baseline/SUPPORT.md`](kernel-baseline/SUPPORT.md)
  for the full support matrix.
* **No `CONFIG_RUST` requirement.** The driver is C against the stable
  kernel-module ABI.

## Build & install

### Via the project's Docker pipeline (recommended)

```sh
tools/build.sh driver           # builds rpbridge.ko in dist/driver/
```

### From source, against your running kernel

```sh
make                                  # builds against /lib/modules/$(uname -r)/build
sudo make modules_install             # installs into /lib/modules/$(uname -r)/extra
sudo depmod -a
sudo modprobe rpbridge
```

### Via DKMS

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
├── Kbuild                   # module object list
├── Makefile                 # out-of-tree build wrapper
├── Kconfig                  # for eventual upstream submission
├── dkms.conf.in             # DKMS recipe template
├── src/
│   ├── main.c               # USB driver entry, probe/disconnect
│   ├── rpbp.h               # wire-protocol constants
│   ├── rpbp_framing.[ch]    # header encode/parse, frame build/validate
│   ├── rpbp_crc32c.[ch]     # Castagnoli CRC-32C lookup table
│   ├── transport.[ch]       # synchronous round-trip over bulk pair
│   ├── cap.[ch]             # CBOR capability parser
│   └── rpbridge.h           # shared per-device state struct
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
# Static analysis with the kernel tree's checkpatch.pl
make checkpatch

# Compile against the running kernel and dist-stage the .ko
tools/build.sh driver

# umockdev replay of recorded USB sessions (planned)
./tools/scripts/run-umockdev.sh
```

## Why C

The driver was first attempted in Rust-for-Linux. R4L's API is still
stabilising — between Linux 6.18 and 7.0 the `kernel::usb::Driver`
trait was restructured, `KString` removed, the `#[vtable]` mechanism
dropped, and the entire probe lifecycle moved to a `PinInit`-style
construction model. Out-of-tree R4L modules pay this drift cost on
every major kernel release for as long as the API surface stays
unstable.

For a driver that needs to land on **whatever Linux Ubuntu ships
today** — which is currently 7.0, will be 8.x within a year, and so
on — Linux's stable kernel-module ABI guarantee for C makes the
maintenance equation flip: a C driver written against the published
kernel headers compiles unchanged across years of kernel releases.
We took that win.

The earlier Rust scaffold remains available on the
`archive/rust-driver` git branch for anyone who wants to revisit the
question once R4L stabilises further. ADR-0003
([`docs/adr/0003-language-choice.md`](../docs/adr/0003-language-choice.md))
will track the eventual reconsideration.
