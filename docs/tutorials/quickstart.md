# Quickstart

Zero to first blink on a Linux workstation running a Rust-enabled
kernel (≥ 6.13).

## 1. Clone with submodules

```sh
git clone --recurse-submodules https://github.com/bauer-group/EMB-USBMultiBusBridge.git
cd USB-Multi-Bus-Bridge
```

## 2. Install host toolchains

```sh
./tools/setup-dev-env.sh
```

This installs `gcc-arm-none-eabi`, `cmake`, `ninja-build`, `rustup`,
`dkms`, `libusb-1.0-0-dev`, `libudev-dev` and `mkdocs-material`
with the versions the repo is tested against.

## 3. Build the firmware

```sh
cmake -S firmware -B firmware/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/build --parallel
```

Expected output: `firmware/build/rpbridge.uf2` (about 180 kB).

## 4. Flash the firmware

Hold `BOOTSEL` on the device while plugging in the USB-C cable. A
`RPI-RP2` mass-storage volume appears. Copy the `.uf2` file:

```sh
cp firmware/build/rpbridge.uf2 /run/media/$USER/RPI-RP2/
```

The device reboots automatically into the new firmware. You should
see a pulsing green LED (heartbeat).

## 5. Build and install the Linux driver

```sh
cd driver
make
sudo make modules_install
sudo depmod -a
sudo modprobe rpbridge
cd ..
```

Check `dmesg | tail` — you should see `rpbridge: probing a matched
USB interface`.

## 6. Build the userspace tools

```sh
cd userspace
cargo build --release
./target/release/rpbridge-ctl info
cd ..
```

`rpbridge-ctl info` prints the tool version, the protocol version,
and — once Device::open_first is wired up — the connected device
identity.

## 7. First bus interactions

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

**`modprobe: ERROR: could not insert 'rpbridge': Invalid module format`**
— your kernel does not have `CONFIG_RUST=y`. Upgrade the kernel or
switch distributions; see [`driver/kernel-baseline/SUPPORT.md`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/kernel-baseline/SUPPORT.md).

**`/dev/rpbridge0` is missing after plug-in** — check `dmesg`
for enumeration errors, run `lsusb` and confirm a device with VID
`1209` is listed, reload the `70-rpbridge.rules` udev rule with
`sudo udevadm control --reload && sudo udevadm trigger`.

**I²C scan finds nothing** — check the Qwiic cable, then check that
the device advertises I²C in the capability map
(`rpbridge-ctl info`).
