# Firmware — overview

The RPBridge firmware runs on the RP2354B's two Cortex-M33 cores.
The current build implements the full RPBP v1 protocol on the
control pipe; bus-specific subsystems (I²C, SPI, GPIO, PWM, ADC,
CAN) land in subsequent milestones.

## Responsibility split (target architecture)

| Core   | Runs                                                               |
|--------|--------------------------------------------------------------------|
| Core 0 | TinyUSB event loop, RPBP framing, command dispatch, configuration operations, capability-map serializer, pin broker, non-time-critical work. |
| Core 1 | I/O scheduler for PIO applets, DMA IRQ handling, ADC/CAN stream pumps, periodic TIME_SYNC emission. |

Core 1 comes online with M6 and, as of M6b, runs a drift-free
ADC sampling service feeding the RPBP streaming path. Additional
services (logic capture, TIME_SYNC emitter) register into the same
scheduler as they land.

## Protocol-layer modules

| Path                                  | Purpose                                                |
|---------------------------------------|--------------------------------------------------------|
| `src/rpbp/rpbp.h`                     | Wire constants, enums, header struct, flag helpers.    |
| `src/rpbp/rpbp_framing.{c,h}`         | Header encode/parse, frame build/validate.             |
| `src/rpbp/rpbp_crc32c.{c,h}`          | Castagnoli CRC-32C lookup table.                       |
| `src/rpbp/rpbp_cbor.{c,h}`            | Minimal RFC 8949 writer.                               |
| `src/rpbp/rpbp_dispatch.{c,h}`        | msg_type router + reply builders.                      |
| `src/rpbp/rpbp_transport.{c,h}`       | TinyUSB vendor-class glue, RX state machine, TX.       |
| `src/capabilities/capabilities.{c,h}` | Capability + identity map emitters.                    |
| `src/commands/cmd_sys.{c,h}`          | SYS subsystem handlers (subsys = 0).                   |
| `src/commands/cmd_uart.{c,h}`         | UART subsystem handlers (subsys = 6) — SET_PHY / GET_PHY. |
| `src/commands/cmd_i2c.{c,h}`          | RPBP I²C subsystem (PROBE / XFER / SCAN / SET_FREQ / GET_FREQ). |
| `src/commands/cmd_spi.{c,h}`          | RPBP SPI subsystem (XFER / SET_MODE / SET_FREQ / GET_FREQ / CS_ASSERT / CS_RELEASE). |
| `src/commands/cmd_gpio.{c,h}`         | RPBP GPIO subsystem (LIST / CONFIGURE / SET / GET / SET_MASK / GET_MASK). |
| `src/commands/cmd_pwm.{c,h}`          | RPBP PWM subsystem (CONFIGURE / SET_DUTY / START / STOP / GET_STATE). |
| `src/commands/cmd_adc.{c,h}`          | RPBP ADC subsystem (READ / READ_MANY / GET_REF / TEMP_READ). |
| `src/usb/tusb_config.h`               | TinyUSB build-time configuration.                      |
| `src/usb/usb_descriptors.c`           | Composite device + BOS + vendor control-transfer.      |
| `src/usb/ms_os_20.{c,h}`              | BOS platform capability + MS OS 2.0 descriptor set.    |
| `src/usb/cdc_glue.{c,h}`              | CDC-ACM ↔ UART byte shuttle + TinyUSB class callbacks. |
| `src/peripherals/pin_broker.{c,h}`    | 48-slot GPIO ownership ledger; every subsystem claims its pins here during init. |
| `src/peripherals/uart_hw.{c,h}`       | IRQ-driven HW UART wrapper + optional software RS485 DE. |
| `src/peripherals/uart_pio.{c,h}`      | PIO-backed 8N1 UART (PIO1, two SMs) with software rings. |
| `src/peripherals/uart_phy.{c,h}`      | UART0 phy-bank selector (OFF / TTL / RS232 / RS485).   |
| `src/peripherals/i2c_hw.{c,h}`        | RP2350 HW-I²C wrapper for both bus instances (Qwiic headers). |
| `src/peripherals/spi.{c,h}`           | PL022 SPI wrapper, dual-instance, software CS.         |
| `src/peripherals/gpio.{c,h}`          | User-GPIO subsystem with pin whitelist + 64-bit atomic mask ops. |
| `src/peripherals/pwm.{c,h}`           | 12-slice PWM wrapper, shared between user subsystem and status LED. |
| `src/peripherals/adc.{c,h}`           | RP2350 SAR-ADC wrapper (4 user channels + temperature sensor). |
| `src/peripherals/can.{c,h}`           | CAN 2.0B via vendored can2040 on PIO0 (GP20/21).       |
| `src/usb/gs_usb_glue.{c,h}`           | SocketCAN-compatible gs_usb protocol on vendor interface 1. |
| `src/pio/uart_pio_tx.pio`             | PIO 8N1 TX program (8 cycles / bit).                   |
| `src/pio/uart_pio_rx.pio`             | PIO 8N1 RX program with framing-error IRQ.             |

## System infrastructure

| Concern                                 | Where / value                                          |
|-----------------------------------------|--------------------------------------------------------|
| **stdio** (`printf`)                    | Routed to UART1 on the 5-pin debug header (GP4/GP5). `stdio_usb` is disabled to avoid collision with the hand-built composite descriptor. |
| **Watchdog**                            | Enabled in `main()` with a 3 s timeout; `pause_on_debug = true` so SWD breakpoints do not cause resets. Fed by the main loop every iteration. |
| **Clock**                               | pico-sdk default — 150 MHz system clock, 48 MHz USB clock, XOSC startup delay multiplier set to 64 in the board header. |
| **Reset cause reporting**               | `watchdog_caused_reboot()` is read at boot and surfaced in the UART1 banner line ("[recovered from watchdog reset]"). |
| **Boot2 stage**                         | W25Q080 QSPI flash driver, chosen in `boards/rpbridge_rp2354b.h`. |
| **Windows binding**                     | BOS descriptor advertises an MS OS 2.0 platform capability (UUID D8DD60DF-...). Windows 10 ≥ 1709 auto-binds WinUSB to interfaces 6 and 7 without INF or installer. See `src/usb/ms_os_20.c`. |

## Supported RPBP features today

- **Frame I/O** — bulk RX with resync, CRC verification, bulk TX
  with blocking flush.
- **HELLO handshake** — CBOR identity map reply.
- **PING / PONG / TIME_SYNC** — with `t2` capture and `t3` in the
  response header.
- **RESET_CHANNEL** — clears device-local seq counters.
- **CMD_REQUEST → SYS subsystem** — all 9 opcodes:
  - `GET_CAPABILITIES` (CBOR wrapper),
  - `ECHO`,
  - `REBOOT_BOOTSEL` (uses `reset_usb_boot()` from the ROM),
  - `UPTIME` (u64 since boot),
  - `GET_VBUS_MV` (scaffold: constant 5000 mV until the ADC lands),
  - `SET_LED` (override the default heartbeat),
  - `SELFTEST` (no tests registered yet — empty pass_mask),
  - `GET_IDENTITY` (compact CBOR identity map),
  - `RESET` (soft-reboot via `watchdog_reboot()`).
- **Error frames** — ERROR with `(status, orig_channel, orig_seq,
  optional reason)` on any bad frame or unsupported request.
- **Stats counters** — `rpbp_transport_stats()` reports
  `rx_frames_ok`, `rx_frames_bad`, `rx_resyncs`, `tx_frames`,
  `tx_drops`. Exposed to the host once SELFTEST opcode payloads
  include counters (pending).

## RX state machine

```text
  ┌───────┐  0x52 seen  ┌────────┐  16 B + valid  ┌──────┐
  │ HUNT  │───────────▶ │ HEADER │───────────────▶│ BODY │
  └───────┘             └────────┘                └──────┘
     ▲     byte not 0x52    │  invalid header          │  full frame
     │  (resync counter++)   │  + ERROR reply           │   │
     └──────────────────────┘                           │   ▼
                                                       CRC OK → dispatch
                                                       CRC bad → ERROR + HUNT
```

On any parse error the buffer is shifted one byte and scanning
resumes at the next potential MAGIC (`0x52`). This keeps the
transport robust against a single lost/corrupted byte without a
full disconnect.

## Build system

- `cmake` ≥ 3.28 + Ninja.
- `arm-none-eabi-gcc` (Launchpad or Ubuntu package).
- `pico-sdk` pinned at tag **2.2.0** as a git submodule.
- `can2040` vendored as a git submodule (compiled conditionally).

```sh
git submodule update --init --recursive firmware/third_party/pico-sdk \
                                        firmware/third_party/can2040
cmake -S firmware -B firmware/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/build --parallel
```

## Host-side unit tests

The protocol modules that have no SDK dependency (`rpbp_framing`,
`rpbp_crc32c`, `rpbp_cbor`) compile on any host toolchain as a
standalone CMake project under [`firmware/tests/host/`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/tree/main/firmware/tests/host):

```sh
cmake -S firmware/tests/host -B firmware/tests/host/build
cmake --build firmware/tests/host/build
ctest --test-dir firmware/tests/host/build --output-on-failure
```

Coverage matches `rpbridge-protocol` on the Rust side, so both
implementations of the wire format stay in lockstep.

## Capability map

The firmware emits a CBOR capability map on every
`CMD_REQUEST{GET_CAPABILITIES}`. For the v1 scaffold the map
reports:

- `proto` = `[1, 0, 0]`
- `fw` = git-describe version
- `board` = `"rpbridge-rev-B"`
- `serial` = 8-byte flash unique ID
- `mtu` = `{ "out": 64, "in": 64 }`
- `max_msg_len` = 4096
- `max_streams` = 16
- `max_reassembled` = 65536
- `features` = `["cbor", "credit-fc", "events", "time-sync", "crc32c.sw", "firmware-update"]`
- `buses` = `{}` (populated as subsystem handlers come online)
- `ota` = `{ "mechanism": "uf2_bootsel", "signature": "minisign-ed25519" }`

Full schema: [`../protocol/capabilities.md`](../protocol/capabilities.md).

## What is still open

See [`firmware/TODO.md`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/firmware/TODO.md)
for the full milestone list. The short version:

- M2 — I²C subsystem (pin broker + HW-I²C + `cmd_i2c`)
- M3 — SPI / GPIO / PWM / ADC
- M4 — CDC-to-UART bridge + RS485 DE applet
- M5 — CAN via can2040 + gs_usb glue
- M6 — Core 1 + streaming channels
- M7 — MS OS 2.0 descriptors for Windows
- M8 — Golden vector corpus + on-device self-test

The protocol layer (M1) is **complete** as of this document.
