# Firmware — overview

The RPBridge firmware runs on the RP2354B's two Cortex-M33 cores with
the following responsibilities split:

| Core   | Runs                                                               |
|--------|--------------------------------------------------------------------|
| Core 0 | TinyUSB event loop, RPBP framing, command dispatch, configuration operations, capability-map serializer, pin broker, non-time-critical work. |
| Core 1 | I/O scheduler for PIO applets, DMA IRQ handling, ADC/CAN stream pumps, periodic TIME_SYNC emission. |

Inter-core traffic uses two lock-free SPSC ring buffers in shared
SRAM; the hardware SIO FIFO is used only for wake signalling, not for
payload — it is just 8 entries deep.

## Build system

* `cmake` ≥ 3.28 + Ninja.
* `arm-none-eabi-gcc` (Launchpad or Ubuntu package).
* `pico-sdk` pinned at tag **2.2.0** as a git submodule.
* `can2040` pinned as a git submodule (master, commit-locked via
  `git submodule update --init`).

```sh
git submodule update --init --recursive firmware/third_party/pico-sdk \
                                        firmware/third_party/can2040
cmake -S firmware -B firmware/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/build --parallel
```

Produces `firmware/build/rpbridge.uf2` plus `.elf`, `.bin`, `.hex`,
`.dis` and a `size.txt` report.

## Source layout

| Path                               | Purpose                                                 |
|------------------------------------|---------------------------------------------------------|
| `firmware/boards/rpbridge_rp2354b.h` | `PICO_BOARD=rpbridge_rp2354b` pin-map and SDK defaults. |
| `firmware/cmake/pico_sdk_import.cmake` | Thin wrapper that resolves the SDK from the vendored submodule. |
| `firmware/src/main.c`              | Entry point, LED heartbeat, TinyUSB task loop.          |
| `firmware/src/rpbp/`               | RPBP framing library: `rpbp.h`, framing, CRC-32C.       |
| `firmware/src/usb/`                | TinyUSB configuration + descriptors (CDC-ACM ×3 + vendor ×2). |
| `firmware/src/periph/`             | HW peripheral drivers (I²C, SPI, UART, PWM, ADC, GPIO). |
| `firmware/src/pio/`                | PIO applets — can2040 glue, RS485 DE, 1-Wire, WS2812.   |
| `firmware/src/cap/`                | Capability-map CBOR serializer.                         |
| `firmware/src/cmd/`                | Per-subsystem command handlers.                         |
| `firmware/src/core1/`              | Core-1 scheduler + IPC queue helpers.                   |
| `firmware/third_party/pico-sdk/`   | Pinned SDK (submodule).                                 |
| `firmware/third_party/can2040/`    | Pinned can2040 (submodule).                             |
| `firmware/tests/host/`             | Host-toolchain unit tests for framing + CRC.            |

## Capability map

On boot, the firmware computes the capability map once (content is
derived from compile-time constants and the PIO applet registry) and
caches the serialized CBOR blob in flash-backed memory. The host can
request it via `CMD_REQUEST{ GET_CAPABILITIES }` on the control
channel at any time; the response carries the cached blob verbatim.

See [`protocol/capabilities.md`](../protocol/capabilities.md) for the
schema.

## Pin broker

Every subsystem activation goes through the pin broker
(`periph/pin_broker.c`). The broker keeps a single source of truth
for pin ownership, rejects conflicts with `EBUSY`, and feeds the
capability serializer. Firmware code MUST NOT touch a GPIO directly —
always `pin_broker_claim()` first, `pin_broker_release()` on tear down.

## Status LED

Single discrete RGB LED on GP22/GP23/GP24. Common-anode; GPIOs sink.
Firmware drives a small state machine encoding:

| Colour              | Meaning                                 |
|---------------------|-----------------------------------------|
| Green pulsing       | System OK, USB active (heartbeat).      |
| Green blinking      | Bus traffic on any subsystem.           |
| Yellow / orange     | CAN error-passive or RS485 timeout.     |
| Red                 | Bus-off or firmware fault.              |
| Blue                | USB suspended.                          |
| Violet / magenta    | OTA / BOOTSEL mode.                     |
| Brief white flash   | User interaction / host-command ACK.    |

## Scope of the scaffold

The checked-in scaffold brings up TinyUSB enumeration with the
composite descriptor, initialises the status LED, and leaves the
rest as TODO with a clearly ordered roadmap in
[`firmware/TODO.md`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/firmware/TODO.md).
