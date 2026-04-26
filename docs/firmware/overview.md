# Firmware — overview

The RPBridge firmware runs on the dual Cortex-M33 cores of the
RP2350 family and ships in two board variants from a single source
tree: the **Rev-B custom PCB (`rpbridge_rp2354b`, QFN-80, 48 GPIO)**
and the **off-the-shelf Raspberry Pi Pico 2
(`rpbridge_pico2`, QFN-60, 30 GPIO)**. Both targets implement the
complete RPBP v1 control surface and all eleven subsystems
(I²C, SPI, UART, GPIO, PWM, ADC, CAN, WS2812 LED, DMX-512,
1-Wire, I²S); the per-board feature flags in the CBOR capability
map are the only thing the host needs to consult to know which
pins exist.

> **Where to look first:** the live coverage matrix lives in
> [`firmware/COVERAGE.md`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/firmware/COVERAGE.md)
> (per-subsystem ship status, evidence links, both-board green/red
> per opcode). Per-pin capability — including which silicon mux
> slots reach which peripheral instance — is documented in the
> [pin capability matrix](../hardware/pin-matrix.md).

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
| `src/rpbp/rpbp.h`                     | Wire constants, enums, header struct, flag helpers, dynamic stream-channel table. |
| `src/rpbp/rpbp_framing.{c,h}`         | Header encode/parse, frame build/validate.             |
| `src/rpbp/rpbp_crc32c.{c,h}`          | Castagnoli CRC-32C lookup table.                       |
| `src/rpbp/rpbp_cbor.{c,h}`            | Minimal RFC 8949 writer.                               |
| `src/rpbp/rpbp_dispatch.{c,h}`        | msg_type router + reply builders + STREAM_OPEN / STREAM_CREDIT / STREAM_DATA wiring. |
| `src/rpbp/rpbp_transport.{c,h}`       | TinyUSB vendor-class glue, RX state machine, TX.       |
| `src/capabilities/capabilities.{c,h}` | Capability + identity map emitters (per-board feature flags). |
| `src/commands/cmd_sys.{c,h}`          | SYS subsystem (subsys = 0) — incl. UART_CLAIM/RELEASE arbiter and SELFTEST loopback battery. |
| `src/commands/cmd_uart.{c,h}`         | UART subsystem (subsys = 6) — SET_PHY / GET_PHY.       |
| `src/commands/cmd_i2c.{c,h}`          | I²C subsystem (PROBE / XFER / SCAN / SET_FREQ / GET_FREQ). |
| `src/commands/cmd_spi.{c,h}`          | SPI subsystem (XFER / SET_MODE / SET_FREQ / GET_FREQ / CS_ASSERT / CS_RELEASE). |
| `src/commands/cmd_gpio.{c,h}`         | GPIO subsystem (LIST / CONFIGURE / SET / GET / SET_MASK / GET_MASK). |
| `src/commands/cmd_pwm.{c,h}`          | PWM subsystem (CONFIGURE / SET_DUTY / START / STOP / GET_STATE). |
| `src/commands/cmd_adc.{c,h}`          | ADC subsystem (READ / READ_MANY / GET_REF / TEMP_READ / STREAM_OPEN). |
| `src/commands/cmd_led.{c,h}`          | LED (WS2812) subsystem — strip configure + frame push via PIO. |
| `src/commands/cmd_dmx.{c,h}`          | DMX-512 subsystem — universe push, BREAK/MAB timing via PIO. |
| `src/commands/cmd_onewire.{c,h}`      | 1-Wire subsystem — RESET / WRITE / READ / SEARCH driven by PIO timing. |
| `src/commands/cmd_i2s.{c,h}`          | I²S subsystem — TX/RX configure, sample-rate selection, DMA pump. |
| `src/usb/tusb_config.h`               | TinyUSB build-time configuration.                      |
| `src/usb/usb_descriptors.c`           | Composite device + BOS + vendor control-transfer.      |
| `src/usb/ms_os_20.{c,h}`              | BOS platform capability + MS OS 2.0 descriptor set.    |
| `src/usb/cdc_glue.{c,h}`              | CDC-ACM ↔ UART byte shuttle, per-PHY arbiter, TinyUSB class callbacks. |
| `src/peripherals/pin_broker.{c,h}`    | 48-slot GPIO ownership ledger; every subsystem claims its pins here during init. Idempotent same-owner re-claim. |
| `src/peripherals/uart_hw.{c,h}`       | IRQ-driven HW UART wrapper + optional software RS485 DE. |
| `src/peripherals/uart_pio.{c,h}`      | PIO-backed 8N1 UART (PIO1, two SMs) with software rings. |
| `src/peripherals/uart_phy.{c,h}`      | UART0 phy-bank selector (OFF / TTL / RS232 / RS485).   |
| `src/peripherals/i2c_hw.{c,h}`        | RP2350 HW-I²C wrapper for both bus instances (Qwiic headers). |
| `src/peripherals/spi.{c,h}`           | PL022 SPI wrapper, dual-instance, software CS.         |
| `src/peripherals/gpio.{c,h}`          | User-GPIO subsystem with pin whitelist + 64-bit atomic mask ops. |
| `src/peripherals/pwm.{c,h}`           | 12-slice PWM wrapper, shared between user subsystem and status LED. |
| `src/peripherals/adc.{c,h}`           | RP2350 SAR-ADC wrapper + Core-1 IIO streamer (drift-free sampling). |
| `src/peripherals/can.{c,h}`           | CAN 2.0B via vendored can2040 on PIO0.                 |
| `src/peripherals/led_ws2812.{c,h}`    | WS2812 strip driver (PIO program + DMA frame push).    |
| `src/peripherals/dmx.{c,h}`           | DMX-512 driver (PIO BREAK/MAB timing + RS485 DE).      |
| `src/peripherals/onewire.{c,h}`       | 1-Wire master via PIO bit-banging.                     |
| `src/peripherals/i2s.{c,h}`           | I²S TX/RX via PIO + DMA.                               |
| `src/usb/gs_usb_glue.{c,h}`           | SocketCAN-compatible gs_usb protocol on vendor interface 1. |
| `src/pio/uart_pio_tx.pio`             | PIO 8N1 TX program (8 cycles / bit).                   |
| `src/pio/uart_pio_rx.pio`             | PIO 8N1 RX program with framing-error IRQ.             |
| `boards/rpbridge_rp2354b.h`           | Board header — 48 GPIO, defines `RPBRIDGE_HAVE_*` feature flags for the Rev-B PCB. |
| `boards/rpbridge_pico2.h`             | Board header — 30 GPIO, narrows feature flags for the off-the-shelf Pico 2 (ADR-0008). |

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
- **All 11 RPBP subsystems** — SYS, I²C, SPI, GPIO, PWM, ADC, UART,
  LED (WS2812), DMX-512, 1-Wire, I²S; CAN goes through the
  vendor-class `gs_usb` glue (mainline driver, no RPBP wrapper
  needed). See [`firmware/COVERAGE.md`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/firmware/COVERAGE.md)
  for the per-opcode evidence matrix on both boards.
- **SYS subsystem** — full opcode set incl. `GET_CAPABILITIES`,
  `ECHO`, `REBOOT_BOOTSEL`, `UPTIME`, `GET_VBUS_MV` (real ADC),
  `SET_LED`, `SELFTEST` (10 on-device loopback tests),
  `GET_IDENTITY`, `RESET`, plus the `UART_CLAIM` / `UART_RELEASE`
  arbiter that prevents CDC-ACM and the RPBP UART subsystem from
  fighting over the same PHY.
- **Streaming channels** — dynamic `STREAM_OPEN` / `STREAM_DATA`
  / `STREAM_CREDIT` plumbing, currently driving the ADC IIO pump
  on Core 1.
- **Error frames** — ERROR with `(status, orig_channel, orig_seq,
  optional reason)` on any bad frame or unsupported request.
- **Stats counters** — `rpbp_transport_stats()` reports
  `rx_frames_ok`, `rx_frames_bad`, `rx_resyncs`, `tx_frames`,
  `tx_drops`; exposed to the host through the `SELFTEST` payload.

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
`CMD_REQUEST{GET_CAPABILITIES}`. The map self-describes everything
the host needs to bind drivers — the host never has to maintain a
firmware-version matrix. Top-level keys:

- `proto` = `[1, 0, 0]`
- `fw` = git-describe version
- `board` = `"rpbridge-rev-B"` or `"rpbridge-pico2"` (build-time)
- `serial` = 8-byte flash unique ID
- `mtu` = `{ "out": 64, "in": 64 }`
- `max_msg_len` = 4096
- `max_streams` = 16
- `max_reassembled` = 65536
- `features` = `["cbor", "credit-fc", "events", "time-sync", "crc32c.sw", "firmware-update", "selftest", "stream-channels"]`
- `buses` = per-subsystem instance map (I²C bus count, SPI bus
  count, ADC channel count, LED strip count, …) gated by the
  per-board `RPBRIDGE_HAVE_*` flags
- `ota` = `{ "mechanism": "uf2_bootsel", "signature": "minisign-ed25519" }`

Reserved for the upcoming M11 surface: `pin.flex` (atomic
pin-function reassignment) and `counter.pio` (PIO2-driven
high-speed edge counter). See
[ADR-0009](../adr/0009-pin-function-flexibility.md).

Full schema: [`../protocol/capabilities.md`](../protocol/capabilities.md).
Per-pin capability for both boards is documented in the
[pin capability matrix](../hardware/pin-matrix.md).

## What is still open

The firmware feature set is complete; the remaining gates are
operational, not functional. See
[`firmware/COVERAGE.md`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/firmware/COVERAGE.md)
for the live status, and
[`firmware/TODO.md`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/firmware/TODO.md)
for the developer working backlog. Open pre-production gates:

- **VID/PID** — currently a pid.codes development PID; needs the
  final BAUER GROUP USB-IF allocation before mass production.
- **`bcdDevice`** — needs to track release tags via build-time
  injection.
- **udev rules** — final naming + group ownership policy.
- **M11** — atomic pin reassignment + high-speed counter
  (PIN / COUNTER subsystems, see ADR-0009).
