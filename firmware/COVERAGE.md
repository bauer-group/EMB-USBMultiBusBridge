# Firmware — coverage matrix

**At-a-glance answer to "what is implemented, what is tested, and
what's still open?"** Read top-down for a one-screen status report;
drill into the per-subsystem rows for evidence (links to source +
spec + tests).

> **Audience.** This page is the *consumer* view of firmware
> readiness — written for users, integrators, packagers, and code
> reviewers. The granular *developer* backlog (with task-level
> checkboxes still in flight) lives in
> [`TODO.md`](TODO.md). The two are kept in sync as part of every
> PR that closes a milestone.

---

## Headline

| Metric                                   | Value     |
|:-----------------------------------------|:----------|
| Subsystems shipped                       | **11 / 11** |
| Subsystems with on-device SELFTEST       | **10 / 11** *(SELFTEST itself excepted)* |
| Subsystems with host-side roundtrip test | **11 / 11** *(via `test_golden`)* |
| Driver wire layer                        | **11 / 11** *(see `driver/src/*.rs`)* |
| Driver kernel-adapter layer              | **🚧 R4L bindings stabilising** *(see [ADR-0007](../docs/adr/0007-ffi-bindings-interim.md))* |
| Build status (rev-B + Pico 2)            | **✅ green** *(per-variant CI matrix)* |
| Pre-production gates                     | **3 open** — see [#Pre-production](#pre-production-gates) |
| Open optional follow-ups                 | **8** — see [TODO.md backlog](TODO.md) |

---

## Subsystem coverage

Legend: ✅ done · 🚧 partial · ❌ not yet · — not applicable

| Subsystem | Firmware | Wire spec | Cap-map flag | Host test | SELFTEST | Driver wire | Driver adapter | rev-B | Pico 2 |
|:----------|:--------:|:----------|:-------------|:---------:|:--------:|:-----------:|:--------------:|:-----:|:------:|
| **SYS** (subsys 0)     | ✅ [`cmd_sys.c`](src/commands/cmd_sys.c)         | [sys.md](../docs/protocol/subsystems/sys.md)         | always-on             | ✅ | ✅ | ✅ | — | ✅ | ✅ |
| **I²C** (subsys 1)     | ✅ [`cmd_i2c.c`](src/commands/cmd_i2c.c) + [`peripherals/i2c_hw.c`](src/peripherals/i2c_hw.c) | [i2c.md](../docs/protocol/subsystems/i2c.md) | `i2c.100k` `.400k` `.1m` | ✅ | ✅ | ✅ | 🚧 | ✅ 2× | ✅ 1× |
| **SPI** (subsys 2)     | ✅ [`cmd_spi.c`](src/commands/cmd_spi.c) + [`peripherals/spi.c`](src/peripherals/spi.c) | [spi.md](../docs/protocol/subsystems/spi.md) | `spi.mode-0-3`        | ✅ | ✅ | ✅ | 🚧 | ✅ 2× | ✅ 1× |
| **GPIO** (subsys 3)    | ✅ [`cmd_gpio.c`](src/commands/cmd_gpio.c) + [`peripherals/gpio.c`](src/peripherals/gpio.c) | [gpio.md](../docs/protocol/subsystems/gpio.md) | `gpio.mask-atomic`    | ✅ | — | ✅ | 🚧 | ✅ 11 pins | ✅ 8 pins |
| **PWM** (subsys 4)     | ✅ [`cmd_pwm.c`](src/commands/cmd_pwm.c) + [`peripherals/pwm.c`](src/peripherals/pwm.c)   | [pwm.md](../docs/protocol/subsystems/pwm.md) | `pwm.invert`          | ✅ | ✅ | ✅ | 🚧 | ✅ 6 ch | ✅ 6 ch |
| **ADC** (subsys 5)     | ✅ [`cmd_adc.c`](src/commands/cmd_adc.c) + [`peripherals/adc.c`](src/peripherals/adc.c)   | [adc.md](../docs/protocol/subsystems/adc.md) | `adc.temp` `adc.stream` | ✅ | ✅ | ✅ | 🚧 | ✅ 4 ch + temp | ✅ 4 ch + temp |
| **UART** (subsys 6)    | ✅ [`cmd_uart.c`](src/commands/cmd_uart.c) + [`peripherals/uart_hw.c`](src/peripherals/uart_hw.c) + [`uart_pio.c`](src/peripherals/uart_pio.c) + [`uart_phy.c`](src/peripherals/uart_phy.c) | [uart.md](../docs/protocol/subsystems/uart.md) | `uart.ttl/.rs232/.rs485/.pio.8n1-only` | ✅ | — | ✅ | — *(stays CDC, ADR-0005)* | ✅ 4 CDC | ✅ 1 TTL |
| **LED** (subsys 7)     | ✅ [`cmd_led.c`](src/commands/cmd_led.c) + [`peripherals/led_strip.c`](src/peripherals/led_strip.c) | [led.md](../docs/protocol/subsystems/led.md) | `led.ws2812`          | ✅ | — | ✅ | 🚧 | ✅ | ✅ |
| **DMX** (subsys 8)     | ✅ [`cmd_dmx.c`](src/commands/cmd_dmx.c) + [`peripherals/dmx.c`](src/peripherals/dmx.c)   | [dmx.md](../docs/protocol/subsystems/dmx.md) | `dmx.tx`              | ✅ | — | ✅ | 🚧 | ✅ | ✅ |
| **1-Wire** (subsys 9)  | ✅ [`cmd_onewire.c`](src/commands/cmd_onewire.c) + [`peripherals/onewire.c`](src/peripherals/onewire.c) | [onewire.md](../docs/protocol/subsystems/onewire.md) | `onewire.master` | ✅ | — | ✅ | 🚧 | ✅ | ✅ |
| **I²S** (subsys 10)    | ✅ [`cmd_i2s.c`](src/commands/cmd_i2s.c) + [`peripherals/i2s.c`](src/peripherals/i2s.c)   | [i2s.md](../docs/protocol/subsystems/i2s.md) | `i2s.tx`              | ✅ | — | ✅ | 🚧 | ✅ | ✅ |
| **PIN** (subsys 11)    | ❌ M11 (planned)                                  | [ADR-0009](../docs/adr/0009-pin-function-flexibility.md) | `pin.flex` *(reserved)* | ❌ | — | ❌ | ❌ | M11 | M11 |
| **COUNTER** (subsys 12)| ❌ M11c (planned)                                 | [ADR-0009](../docs/adr/0009-pin-function-flexibility.md) | `counter.pio` *(reserved)* | ❌ | — | ❌ | ❌ | M11c | M11c |
| **CAN** *(via gs_usb)* | ✅ [`peripherals/can.c`](src/peripherals/can.c) + [`usb/gs_usb_glue.c`](src/usb/gs_usb_glue.c) | [hardware-canfd.md](../docs/protocol/hardware-canfd.md) | `can.classic` `can.gs-usb` `can.hw-timestamp` | — *(no RPBP path)* | ✅ | — *(mainline `gs_usb`)* | — *(mainline)* | ✅ | ✅ |

**Driver-adapter status legend** (🚧 column):

| Driver adapter status | Meaning                                                                                         |
|:----------------------|:------------------------------------------------------------------------------------------------|
| ✅                    | aux_device child registered + kernel-side ABI calls land on the firmware                         |
| 🚧                    | wire layer + Client struct done in Rust ([`driver/src/<sys>.rs`](../driver/src/)); aux child blocked on R4L bindings stabilising in mainline (ADR-0007) |
| ❌                    | not implemented yet                                                                              |

The 🚧 marker is **not** a firmware concern — the firmware presents
a stable RPBP surface that any client can drive (the Rust client
already does, via libusb in tests).

---

## Per-board capability matrix

| Capability                         | rev-B (`rpbridge_rp2354b`)               | Pi Pico 2 (`rpbridge_pico2`)              |
|:-----------------------------------|:-----------------------------------------|:------------------------------------------|
| Package / GPIO count               | RP2350B QFN-80, 48 GPIO                  | RP2350A QFN-60, 30 GPIO                   |
| Flash / RAM                        | 2 MB QSPI / 520 kB SRAM                  | 4 MB QSPI / 264 kB SRAM                   |
| **CDC-ACMs**                       | 4 (RS232 / RS485 / TTL / PIO-UART) †     | 2 (TTL HW / PIO-UART) †                   |
| **UART phy-bank** (RS232/RS485/TTL)| ✅ via on-board MAX3232 + SN65HVD72       | ❌ external transceiver required           |
| **I²C** instances                  | 2 (Qwiic 1+2 + Qwiic 3 + 4-pin header)   | 1 (header)                                |
| **SPI** instances                  | 2 (PMOD + breakout, 3 CS lines)          | 1 (PMOD-style header)                     |
| **User GPIO** (broker-arbitrated)  | 11 (GP30/31/37–45)                       | 8 (GP8–15)                                |
| **PWM** (user-exposed)             | 6 (GP30/31/37–40)                        | 6 (GP8–13)                                |
| **ADC** (external + temp)          | 4 + 1 (GP26–29 + temp)                   | 4 + 1                                     |
| **CAN 2.0B**                       | ✅ on-board SN65HVD230                    | ✅ external transceiver                    |
| **WS2812 / SK6812 LED strip**      | ✅ DMA-driven, up to 256 px               | ✅                                         |
| **DMX-512 TX**                     | ✅ XLR-5 via shared SN65HVD72             | ✅ external                                |
| **1-Wire**                         | ✅ dedicated header                       | ✅ (shares user-GPIO)                      |
| **I²S TX**                         | ✅ 5-pin header                           | ✅ (shares user-GPIO)                      |
| **Status LED**                     | ✅ discrete RGB on PWM                    | ✅ single green on GP25                    |
| **User button**                    | ✅ front panel                            | ✅ BOOTSEL                                 |

† — A 5th CDC carrying `printf()` output ("RPBridge Debug Log")
can be opted into at build time with `-DRPBRIDGE_DEBUG_CDC=ON`.
Default builds keep the production layout; see
[firmware/README.md #Debugging](README.md#debugging) for the
toggle and the descriptor-shift it triggers.

---

## Test infrastructure

| Tier                                  | Coverage                                                 | Source                                              |
|:--------------------------------------|:---------------------------------------------------------|:----------------------------------------------------|
| Wire format roundtrip (host)          | 13 cases × every subsystem                               | [`tests/host/test_golden.c`](tests/host/test_golden.c) |
| Framing edge-cases (host)             | 9 negative-path cases (CRC, length, reserved bits, …)    | [`tests/host/test_framing.c`](tests/host/test_framing.c) |
| CRC-32C reference vectors (host)      | RFC 3720 + bit-flip                                      | [`tests/host/test_crc32c.c`](tests/host/test_crc32c.c) |
| CBOR writer (host)                    | full RFC 8949 subset                                     | [`tests/host/test_cbor.c`](tests/host/test_cbor.c)  |
| Dispatcher round-trip (host, pico-sdk-stubbed) | 7 cases: HELLO/PING/CMD/RESET/wrong-direction/unknown-msg | [`tests/host/test_sys_dispatch.c`](tests/host/test_sys_dispatch.c) |
| Byte-frozen golden corpus generator   | 7 canonical wire vectors → checked-in `.hex`             | [`tests/host/gen_golden.c`](tests/host/gen_golden.c) + [`tests/golden/`](tests/golden/) |
| **On-device SELFTEST opcode**         | 10 live tests covering pin broker, CRC, clk_sys, ADC temp, I²C, SPI, PWM, CAN, Core 1 liveness, watchdog | [`cmd_sys.c::op_selftest`](src/commands/cmd_sys.c)  |
| Per-variant build matrix              | rev-B + Pico 2 builds in CI on every PR                  | [`.github/workflows/firmware.yml`](../.github/workflows/firmware.yml) |

---

## Resource budget at a glance

| Resource           | Used                          | Free                             | Source                                                    |
|:-------------------|:------------------------------|:---------------------------------|:----------------------------------------------------------|
| USB endpoints (Cfg 1) | 10 IN + 6 OUT                 | 5 IN + 9 OUT                     | [resource-budget #2](../docs/firmware/resource-budget.md#2-usb-endpoints) |
| PIO state machines    | 9 / 12                        | 3 (PIO2 fully open after I²S)    | [resource-budget #3](../docs/firmware/resource-budget.md#3-pio-blocks-state-machines-instructions) |
| PIO instructions      | ~74 / 96 across blocks        | ~22 free                         | (same)                                                    |
| DMA channels          | dynamic via `dma_claim_unused_channel()` | 16 total — never saturated         | [resource-budget #4](../docs/firmware/resource-budget.md#4-dma-channels) |
| Flash (rev-B build)   | ≈ 110 kB code + 50 kB rodata  | of 2 MB                          | linker map per build                                      |

---

## Pre-production gates

These items **must** be closed before any public hardware shipment.
Tracked here separately from the engineering backlog so they don't
get lost in the noise of optional follow-ups.

- ✅ **USB VID/PID** — production allocation `0545:0101` (BAUER GROUP
  / RPBridge) wired in firmware, driver, userspace defaults, and
  udev rules on 2026-04-27. See [`docs/usb-vid-pid.md`](../docs/usb-vid-pid.md)
  for the lock-step list + interface-class layout.
- ❌ **`bcdDevice`** — bump from `0x0100` to mark the first
  production-ID release.
- ❌ **udev rules + umockdev recordings** — regenerate after the
  real PID lands so distribution packages match.

---

## Backlog (developer workspace)

Granular task tracking — items in flight, optional follow-ups, and
the multi-board polish that didn't make the M9 cut — lives in
[`TODO.md`](TODO.md). The currently-open backlog items are:

- **PIO-precision RS485 DE** *(future, tracked since M4d)* — only
  if a scope-level audit shows the software DE has unacceptable
  jitter at 3 Mbaud.
- **Multi-board CI grep guards + LED-mono variant polish** —
  cosmetic build-time fences against pin-number drift.
- **DMX RX (RDM) + DMX phy reuse with UART0 RS485** — feature
  growth tracked separately.
- **SK6812-RGBW** — 32-bit/pixel variant of the WS2812 program.
- **I²S continuous streaming via RPBP stream channel** — rides on
  M6b infrastructure once a use-case lands.
- **1-Wire overdrive (10 µs slot)** — needs PIO clkdiv tweak.

The above are explicitly *post-1.0* and do not affect the
"firmware feature-complete" claim above.

---

## Provenance

This page is generated from the same review checklist that lands
the milestone PRs — every ✅ marker is backed by a merged commit
referenced from the per-subsystem source link. To verify a row,
follow the source-file link, then the spec link, then the test
link. If any of the three is broken or stale, that's a CI bug —
flag it on the row instead of silently working around it.

Last touched: see `git log -- firmware/COVERAGE.md`.
