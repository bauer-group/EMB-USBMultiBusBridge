# RPBridge — firmware

Raspberry Pi **RP2354B** firmware, built on the official **pico-sdk 2.2.0**
(pinned as a git submodule) and **TinyUSB** (bundled with the SDK).
CAN 2.0B is implemented entirely in PIO via Kevin O'Connor's
[can2040](https://github.com/KevinOConnor/can2040) library, also vendored
as a pinned submodule.

## Prerequisites

| Tool                     | Minimum | Note                                       |
|--------------------------|---------|--------------------------------------------|
| CMake                    | 3.28    | The SDK requires ≥ 3.13; we require 3.28 for `file(CONFIGURE …)`. |
| Ninja                    | 1.11    | Faster and simpler than `make -j`.         |
| `gcc-arm-none-eabi`      | 13.2    | Debian/Ubuntu package `gcc-arm-none-eabi`. |
| Python                   | 3.10    | Needed for the SDK's PIO assembler.        |
| `picotool`               | 2.0     | Optional; the SDK pulls it in on demand.   |

## First-time setup

```sh
git submodule update --init --recursive firmware/third_party/pico-sdk \
                                        firmware/third_party/can2040
```

## Build

```sh
cmake -S firmware -B firmware/build \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/build --parallel
```

Produces `firmware/build/rpbridge.uf2`, `.elf`, `.bin`, `.hex` and the
corresponding size report.

## Flash

Hold `BOOTSEL` on the device while plugging in USB — a `RPI-RP2` mass-
storage volume appears. Copy the `.uf2` onto it; the device reboots into
the new firmware automatically.

Alternatively:

```sh
picotool load -fxv firmware/build/rpbridge.uf2
```

Once a previous firmware version is running, the top-level
`rpbridge-ctl flash` subcommand automates the BOOTSEL trigger + flash
+ reconnect dance.

## Layout

| Path                                    | Purpose                          |
|-----------------------------------------|----------------------------------|
| `boards/rpbridge_rp2354b.h`             | Custom PICO_BOARD header         |
| `cmake/pico_sdk_import.cmake`           | Thin wrapper over the vendored SDK |
| `src/main.c`                            | Entry point, LED heartbeat       |
| `src/rpbp/rpbp.h`                       | RPBP v1 protocol definitions     |
| `src/rpbp/rpbp_framing.c`               | Header pack/unpack + validation  |
| `src/rpbp/rpbp_crc32c.{c,h}`            | Castagnoli CRC-32C implementation|
| `src/usb/tusb_config.h`                 | TinyUSB configuration            |
| `src/usb/usb_descriptors.c`             | Composite descriptor set         |
| `third_party/pico-sdk/`                 | Vendored SDK (submodule @ 2.2.0) |
| `third_party/can2040/`                  | Vendored CAN controller (submodule) |

See `TODO.md` for the remaining modules and the milestone order.

## Host-side unit tests

The RPBP framing and CRC-32C code is written to compile on the host
toolchain as well — useful for fast test iteration without flashing.

```sh
cmake -S firmware -B firmware/build-host \
      -DCMAKE_TOOLCHAIN_FILE=cmake/host-toolchain.cmake \
      -DRPBRIDGE_HOST_TESTS=ON
cmake --build firmware/build-host --target test
```

(The host-toolchain file lands together with the `tests/host/` fixtures
— see `TODO.md` §M7.)
