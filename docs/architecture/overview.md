# Architecture — overview

RPBridge is a three-layer system with a narrow wire protocol between
the layers. Each layer is responsible for exactly one thing and can
be tested in isolation.

```text
┌──────────────────────────────────────────────────────────────┐
│                       Linux host                             │
│                                                              │
│  Userspace tools                                             │
│  ┌──────────────┐  ┌───────────┐  ┌─────────┐  ┌──────────┐  │
│  │ i2c-tools    │  │ libgpiod  │  │ can-utils│  │ iio-utils│  │
│  └──────┬───────┘  └─────┬─────┘  └────┬────┘  └────┬─────┘  │
│         │                │              │            │        │
│       /dev/i2c-N  /dev/gpiochipN       can0     iio:deviceN   │
│         │                │              │            │        │
│  ┌──────┴───────┐  ┌─────┴─────┐  ┌────┴────┐  ┌────┴─────┐  │
│  │ i2c-core     │  │ gpiolib   │  │ gs_usb  │  │ iio      │  │
│  └──────┬───────┘  └─────┬─────┘  └────┬────┘  └────┬─────┘  │
│         │                │              │            │        │
│         └────┬───────────┴──── auxiliary_bus ────────┘        │
│              │                                                │
│  ┌───────────┴───────────────────────────────────────────────┐│
│  │          rpbridge.ko (Rust, kernel)                       ││
│  │    USB-if matcher · RPBP demux · CRC-32C · cap parser     ││
│  └───────────────────┬───────────────────────────────────────┘│
└─────────────────────┬│──────────────────────────────────────┘
                       USB 2.0 Full Speed
┌─────────────────────┼────────────────────────────────────────┐
│                     ▼                                        │
│               RP2354B (150 MHz dual M33, 2 MB flash)         │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐    │
│  │ Core 0                                               │    │
│  │   TinyUSB · RPBP framing · command dispatch          │    │
│  │   capability CBOR emitter · pin broker               │    │
│  └──────────────────────────────────────────────────────┘    │
│  ┌──────────────────────────────────────────────────────┐    │
│  │ Core 1                                               │    │
│  │   I/O scheduler · DMA IRQ · PIO service · streams    │    │
│  └──────────────────────────────────────────────────────┘    │
│                                                              │
│  HW peripherals: 2×UART · 2×I²C · 2×SPI · 24×PWM · 8×ADC     │
│  PIO0: can2040 (3 SMs) — CAN 2.0B via software controller    │
│  PIO1: extra UARTs, RS485-DE precision timing                │
│  PIO2: 1-Wire, WS2812, spare applets                         │
│                                                              │
│  Transceivers: SN65HVD230 (CAN) · SN65HVD72 (RS485)          │
│                MAX3232 (RS232) · 3× CDC-ACM (host side)      │
└──────────────────────────────────────────────────────────────┘
```

## Layer boundaries

| Layer            | Lives in            | Language | Talks to            |
|------------------|---------------------|----------|---------------------|
| Userspace tools  | Distro packages     | any      | Kernel subsystems   |
| Userspace Rust   | `userspace/`        | Rust     | USB via `nusb`      |
| Kernel driver    | `driver/`           | Rust     | USB core + subsystems|
| RPBP wire format | `userspace/rpbridge-protocol/`, `firmware/src/rpbp/`, `driver/src/rpbp.rs` | Rust + C | bytes over bulk EP |
| Firmware         | `firmware/`         | C        | RP2354B hardware    |
| Hardware         | `hardware/`         | KiCad    | physics             |

## Why two USB interface families

A single vendor interface would have been enough *theoretically*, but
reusing mainline kernel drivers where they exist saves us real work:

1. **CDC-ACM × 3** — UART 1, UART 2, and the debug log. Handled by
   `usbser` on every OS; every terminal and `tio`-class tool works
   untouched.
2. **gs_usb vendor** — a single CAN interface speaking the gs_usb /
   candleLight protocol. Handled by `drivers/net/can/usb/gs_usb.ko`
   on Linux; handled by `candle_api`, `python-can` and SavvyCAN on
   Windows.
3. **RPBP vendor** — everything else: I²C, SPI, GPIO, PWM, ADC,
   1-Wire, UART phy selection, control plane, OTA. Our own Rust
   driver owns this interface.

## Data-path examples

**I²C read of a BME280**:

1. Userspace calls `read(/dev/i2c-7, buf, 6)`.
2. `i2c-dev` forwards to the `i2c_adapter` registered by the RPBridge
   driver's `rpbridge-i2c` auxiliary child.
3. Child composes a `CMD_REQUEST{ I2C_XFER, bus=0, addr=0x76, len=6 }`
   RPBP frame, CRC-seals it, hands it to the core driver.
4. Core driver submits a bulk-OUT URB, waits on a completion keyed by
   `seq`.
5. Device firmware reads the frame, runs a `i2c_read_timeout_us()` via
   the HW I²C controller (or its PIO equivalent), formats a
   `CMD_RESPONSE`, pushes it back on bulk-IN.
6. Core driver validates CRC, matches `seq`, wakes the waiting i2c
   child, which returns the buffer to i2c-dev.

Round-trip budget: 400 µs on a warm link with short payloads (see
`docs/drafts/rpbridge-protocol-and-modern-stack.md` §3.12).

**GPIO IRQ delivery** is the reverse, asynchronous path: firmware's
GPIO ISR pushes an `EVENT` frame on the events channel (1); driver's
URB completion callback routes it to the right `rpbridge-gpio`
aux child, which calls `generic_handle_irq_safe()` for the line's IRQ
domain. End-to-end p99 budget: 1 ms.
