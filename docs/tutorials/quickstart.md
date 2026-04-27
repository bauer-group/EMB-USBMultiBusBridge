# Quickstart

Zero to first blink on a Linux workstation. The build runs inside a
project-supplied Docker toolchain image so the local-developer path
and the CI path stay byte-identical — no host-side toolchain pinning
gymnastics required.

## 1. Clone with submodules

```sh
git clone --recurse-submodules https://github.com/bauer-group/EMB-USBMultiBusBridge.git
cd EMB-USBMultiBusBridge
```

## 2. Build everything

```sh
./tools/build.sh all
```

The first invocation on a fresh machine takes 5–10 minutes (apt +
rustup inside the container). Every subsequent invocation hits the
docker layer cache and runs in seconds.

Outputs land under `dist/`:

| Path | Description |
| ---- | ----------- |
| `dist/firmware/rp2354b/rpbridge-rp2354b.uf2` | Firmware image for the BAUER GROUP rev-B PCB |
| `dist/firmware/pico2/rpbridge-pico2.uf2` | Firmware image for the bare Raspberry Pi Pico 2 |
| `dist/driver/rpbridge.ko` | Linux kernel module |
| `dist/userspace/rpbridge-ctl` | Userspace CLI |

You can also build pieces individually:

```sh
./tools/build.sh firmware                  # both boards
./tools/build.sh firmware rpbridge_pico2   # one board
./tools/build.sh driver                    # kernel module
./tools/build.sh userspace                 # native (x86_64) build
./tools/build.sh userspace --aarch64       # cross-build to aarch64
```

## 3. Flash the firmware

Hold `BOOTSEL` on the device while plugging in the USB-C cable. A
`RPI-RP2` mass-storage volume appears. Copy the `.uf2` file:

```sh
cp dist/firmware/rp2354b/rpbridge-rp2354b.uf2 /run/media/$USER/RPI-RP2/
```

The device reboots automatically into the new firmware. You should
see a pulsing green LED (heartbeat).

## 4. Install the kernel module

```sh
sudo cp dist/driver/rpbridge.ko /lib/modules/"$(uname -r)"/extra/
sudo depmod -a
sudo modprobe rpbridge
```

Check `dmesg | tail` — you should see `rpbridge: probing matched USB
interface`.

## 5. First bus interactions

```sh
# Scan I²C bus 0 — anything on the Qwiic headers shows up here.
i2cdetect -y 0

# Drive a GPIO high, then low.
gpioset $(gpiodetect | awk '/rpbridge/ {print $1}') 30=1
sleep 0.5
gpioset $(gpiodetect | awk '/rpbridge/ {print $1}') 30=0

# List ADC channels.
iio_info | grep rpbridge -A2

# Bring CAN up and dump traffic.
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
candump can0
```

## Troubleshooting

**`docker: Cannot connect to the Docker daemon`** — ensure Docker
Desktop (Windows / macOS) or the `docker` service (Linux) is
running. The build script assumes a working `docker` binary on
`PATH`.

**`/dev/rpbridge0` is missing after plug-in** — check `dmesg` for
enumeration errors, run `lsusb` and confirm a device with VID
`0545` (BAUER GROUP) is listed, reload the `70-rpbridge.rules` udev
rule with `sudo udevadm control --reload && sudo udevadm trigger`.

**I²C scan finds nothing** — check the Qwiic cable, then check that
the device advertises I²C in the capability map
(`rpbridge-ctl info`).

## Native (no-Docker) workflow

If you want to skip the container hop and use a host-side toolchain
(useful when iterating on a single component):

```sh
./tools/setup-dev-env.sh        # one-time host install
RPBRIDGE_NO_DOCKER=1 ./tools/build.sh firmware rpbridge_pico2
```

The setup script targets Debian/Ubuntu, Fedora and Arch — see
[CONTRIBUTING.md](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/CONTRIBUTING.md)
for the supported distro matrix.
