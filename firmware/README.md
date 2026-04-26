# RPBridge — firmware

Raspberry Pi **RP2354B** firmware, built on the official **pico-sdk**
and **TinyUSB** (vendored as pinned git submodules). CAN 2.0B is
implemented entirely in PIO via Kevin O'Connor's
[`can2040`](https://github.com/KevinOConnor/can2040) library, also a
pinned submodule.

## Pinned dependencies

| Component   | Version / commit                     | Role                                 |
|-------------|--------------------------------------|--------------------------------------|
| pico-sdk    | tag `2.2.0` (`a1438df`)              | Board support, TinyUSB integration.  |
| TinyUSB     | `0.18.0` (ships inside the SDK)      | USB device stack.                    |
| can2040     | commit `2988d4f`                     | Classic CAN 2.0B controller in PIO.  |

Submodule definitions live in [`.gitmodules`](../.gitmodules). Cloning
the outer repo with `--recurse-submodules` pulls in the exact pinned
revisions.

## Prerequisites

| Tool                     | Minimum | Debian / Ubuntu package          |
|--------------------------|---------|----------------------------------|
| `cmake`                  | 3.28    | `cmake`                          |
| `ninja`                  | 1.11    | `ninja-build`                    |
| `arm-none-eabi-gcc`      | 13.2    | `gcc-arm-none-eabi`              |
| ARM newlib               | 13.2    | `libnewlib-arm-none-eabi`        |
| Python                   | 3.10+   | `python3`                        |
| `picotool` (optional)    | 2.0+    | built by the SDK or from source  |

## First-time setup

Fresh clone:

```sh
git clone --recurse-submodules https://github.com/bauer-group/EMB-USBMultiBusBridge.git
cd USB-Multi-Bus-Bridge
```

Existing clone:

```sh
git submodule update --init --recursive \
    firmware/third_party/pico-sdk \
    firmware/third_party/can2040
```

Verify the submodules landed on the pinned commits:

```sh
git submodule status --recursive
#  a1438dff... firmware/third_party/pico-sdk (2.2.0)
#  2988d4f1... firmware/third_party/can2040 (v1.7.0-8-g2988d4f)
#  86ad6e56... firmware/third_party/pico-sdk/lib/tinyusb (0.18.0)
```

## Build

```sh
cmake -S firmware -B firmware/build \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/build --parallel
```

Produces (under `firmware/build/`):

| File               | Purpose                                                |
|--------------------|--------------------------------------------------------|
| `rpbridge.uf2`     | Drag-and-drop target for the ROM BOOTSEL bootloader.   |
| `rpbridge.elf`     | With debug info; for `gdb`, SEGGER, picoprobe.         |
| `rpbridge.bin`     | Raw flash image for external programmers.              |
| `rpbridge.hex`     | Intel HEX.                                             |
| `rpbridge.dis`     | Disassembly (release builds only).                     |

## Flash

Hold `BOOTSEL` on the device while plugging in USB — a `RPI-RP2`
mass-storage volume appears. Copy the `.uf2` onto it.

Alternatively:

```sh
picotool load -fxv firmware/build/rpbridge.uf2
```

Once a previous RPBridge firmware is running, the userspace CLI
automates the BOOTSEL trigger + flash + reconnect:

```sh
rpbridge-ctl flash firmware/build/rpbridge.uf2
```

## Debugging

### Logs

`stdio` is routed to **UART1** on the 5-pin debug header
(`GP4 = TX`, `GP5 = RX`). Connect a USB-serial adapter at 115 200 8N1
to see `printf()` output.

`stdio_usb` is intentionally disabled — enabling it would spawn a
private CDC-ACM interface that collides with our hand-built composite
(3× CDC + 2× vendor). A future revision can expose log output on
CDC-ACM #2 ("rpbridge Debug Log") by writing directly into
`tud_cdc_n_write(2, ...)` from the firmware.

### SWD

The same 5-pin header carries `SWDIO` / `SWCLK` / `GND`. Any
CMSIS-DAP debugger (Picoprobe, J-Link, Black Magic) works with
openocd:

```sh
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
        -c "program firmware/build/rpbridge.elf verify reset exit"
```

### Watchdog

The firmware enables the hardware watchdog with a **3 s timeout**
(`pause_on_debug = true`). If the main loop stalls longer than that —
e.g. a handler deadlocks on a bus — the MCU reboots and the banner
line on UART1 reports *"[recovered from watchdog reset]"*.

When single-stepping in SWD, the watchdog is paused automatically so
breakpoints don't trigger spurious resets.

## Host-side unit tests

The portable RPBP modules (`rpbp_framing`, `rpbp_crc32c`, `rpbp_cbor`)
compile on any host toolchain as a **standalone** CMake project:

```sh
cmake -S firmware/tests/host -B firmware/tests/host/build
cmake --build firmware/tests/host/build
ctest --test-dir firmware/tests/host/build --output-on-failure
```

No pico-sdk, no arm-none-eabi-gcc. See [`tests/host/README.md`](tests/host/README.md).

## USB layout at a glance

```text
VID 0x1209 / PID 0xFFFE (pid.codes testing range)

Interface  Class              Purpose                        Host driver
─────────  ─────────────────  ─────────────────────────────  ─────────────────────────
0,1        CDC-ACM × 1        UART 1 RS232  (via MAX3232)    usbser / inbox VCP
2,3        CDC-ACM × 1        UART 1 RS485  (via SN65HVD72)  usbser / inbox VCP
4,5        CDC-ACM × 1        UART 1 TTL    (direct drive)   usbser / inbox VCP
6,7        CDC-ACM × 1        UART 2 TTL    (PIO-UART #2)    usbser / inbox VCP
8          Vendor (WinUSB)    RPBP control (bulk)            rpbridge.ko / WinUSB
9          Vendor (WinUSB)    gs_usb CAN   (bulk)            gs_usb.ko / candle_api
```

The debug log is NOT on a CDC; `stdio` (printf) goes to UART1 on the
5-pin debug header instead — see the "Debugging" section above.

**Driverless phy selection:** a developer picks the UART0 physical
layer by opening the correspondingly-labelled virtual COM port — the
firmware auto-switches the transceiver (MAX3232 / SN65HVD72 / direct
TTL) based on which of CDC 0/1/2 currently asserts DTR. No rpbridge-
ctl, no libusb, no kernel driver in the hot path. Only one UART0 CDC
may be active at a time; opening a second takes over and drains the
previously-active one. See
[`docs/protocol/subsystems/uart.md`](../docs/protocol/subsystems/uart.md)
for the full semantics and the RPBP override path.

Windows 10 ≥ 1709 binds WinUSB automatically via the BOS + MS OS 2.0
descriptor set (see [`src/usb/ms_os_20.c`](src/usb/ms_os_20.c)).
The two vendor interfaces register distinct `DeviceInterfaceGUIDs`:

| Interface          | DeviceInterfaceGUID                        |
|--------------------|---------------------------------------------|
| 8 — RPBP Control   | `{24D7D4F5-FB9F-4B5D-8A1F-69F3C9FE8B23}`    |
| 9 — gs_usb CAN     | `{8F3B2E41-5A6C-4D7E-9F1A-2B3C4D5E6F70}`    |

## Source tree

```
firmware/
├── CMakeLists.txt
├── cmake/
│   └── pico_sdk_import.cmake        # wraps the vendored SDK
├── boards/
│   └── rpbridge_rp2354b.h           # PICO_BOARD=rpbridge_rp2354b
├── src/
│   ├── main.c                       # TinyUSB + RPBP pump + LED + WDG
│   ├── version.h.in                 # git-describe injection
│   ├── rpbp/                        # RPBP v1 protocol
│   │   ├── rpbp.h
│   │   ├── rpbp_framing.{c,h}
│   │   ├── rpbp_crc32c.{c,h}
│   │   ├── rpbp_cbor.{c,h}
│   │   ├── rpbp_dispatch.{c,h}
│   │   └── rpbp_transport.{c,h}
│   ├── cap/
│   │   └── capabilities.{c,h}       # CBOR capability + identity emitter
│   ├── cmd/
│   │   └── cmd_sys.{c,h}            # SYS subsystem handlers
│   └── usb/
│       ├── tusb_config.h
│       ├── usb_descriptors.c        # composite + BOS + control xfer
│       ├── ms_os_20.{c,h}           # MS OS 2.0 descriptor set
│       └── (cdc_glue, vendor_class, gs_usb_glue — future modules)
├── third_party/
│   ├── pico-sdk/                    # submodule @ tag 2.2.0
│   └── can2040/                     # submodule
├── tests/
│   └── host/                        # standalone host-test project
└── TODO.md                          # milestone checklist
```

## Milestone status

| # | Topic                            | Status        |
|---|----------------------------------|---------------|
| M1 | RPBP v1 end-to-end              | ✅ complete   |
| M2 | Pin broker + HW I²C             | pending       |
| M3 | SPI / GPIO / PWM / ADC          | pending       |
| M4 | CDC ↔ UART phy + RS485 DE       | pending       |
| M5 | CAN (can2040 + gs_usb)          | pending       |
| M6 | Core 1 scheduler + streams      | pending       |
| M7 | MS OS 2.0 descriptors (Windows) | ✅ complete   |
| M8 | Golden vectors + device self-test | pending     |

See [`TODO.md`](TODO.md) for per-module detail.
