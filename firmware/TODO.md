# Firmware — implementation roadmap

Track for each component whether it is implemented (`[x]`) or pending
(`[ ]`). Every entry points to the corresponding module and the
authoritative spec section.

## M1 — RPBP v1 end-to-end

- [x] `src/rpbp/rpbp.h` — protocol constants (msg_type, flags,
      channels, status codes, SYS opcodes)
- [x] `src/rpbp/rpbp_framing.{c,h}` — header encode/parse, frame
      build/validate, reserved-flag-bit check
- [x] `src/rpbp/rpbp_crc32c.{c,h}` — Castagnoli CRC-32C lookup table
- [x] `src/rpbp/rpbp_cbor.{c,h}` — minimal RFC 8949 writer
- [x] `src/rpbp/rpbp_dispatch.{c,h}` — msg_type router + reply
      builders (CMD_RESPONSE, ERROR, PONG, EVENT)
- [x] `src/rpbp/rpbp_transport.{c,h}` — TinyUSB vendor RX/TX state
      machine with resync
- [x] `src/cap/capabilities.{c,h}` — CBOR capability + identity emitters
- [x] `src/cmd/cmd_sys.{c,h}` — SYS subsystem handlers (all 9 opcodes
      from `docs/protocol/subsystems/sys.md`)
- [x] `src/main.c` — TinyUSB + transport pump in the main loop
- [x] `tests/host/` — host-side unit tests for CRC, framing, CBOR

## M2 — HW-I²C + pin broker

- [ ] `src/periph/pin_broker.{c,h}` — single source of truth for GPIO
      ownership; conflicts return `EBUSY`
- [ ] `src/periph/i2c.{c,h}` — `hardware_i2c` wrapper with DMA for
      burst transfers
- [ ] `src/cmd/cmd_i2c.c` — I2C subsystem handlers
- [ ] Update `src/cap/capabilities.c` to advertise the two I²C bus
      entries (`"buses"` → `{"i2c": [...]}`)

## M3 — SPI + GPIO + PWM + ADC

- [ ] `src/periph/spi.{c,h}` + `src/cmd/cmd_spi.c`
- [ ] `src/periph/gpio.{c,h}` + `src/cmd/cmd_gpio.c` (+ IRQ events
      over channel 1)
- [ ] `src/periph/pwm.{c,h}` + `src/cmd/cmd_pwm.c`
- [ ] `src/periph/adc.{c,h}` + `src/cmd/cmd_adc.c` (single-shot + IIO
      streaming)
- [ ] LED brightness via PWM instead of binary GPIO in `main.c`

## M4 — CDC-ACM bridge to HW UART / RS485-DE applet

### M4a — HW UART bridge (CDC #0 <-> UART0)

- [x] `src/periph/uart_hw.{c,h}` — IRQ-driven RX/TX ring buffers
      around the RP2350 HW UART; 2 KB RX / 1 KB TX per instance.
- [x] `src/usb/cdc_glue.{c,h}` — CDC-ACM #0 <-> UART0 HW byte shuttle
      with back-pressure on both sides; TinyUSB callbacks for line
      coding, line state (DTR/RTS), and send-break.
- [x] Capability map advertises `"uart.ttl"` and a single
      `buses.uart` instance (CDC-ACM index 0, HW backend).

### M4b — PIO UART bridge (CDC #1)

- [x] `src/pio/uart_pio_tx.pio` / `uart_pio_rx.pio` — 8N1 PIO
      programs ported from pico-examples, with RX framing-error IRQ.
- [x] `src/periph/uart_pio.{c,h}` — PIO-backed UART with the same
      public API as `uart_hw` (ring buffers, line-coding, tick).
- [x] `cdc_glue.c` bridges CDC-ACM #1 to uart_pio instance 0
      (GP33 TX / GP36 RX on PIO1). Capability map lists the 2nd
      instance under `buses.uart`.

### M4c — UART0 phy-bank switch

- [x] `src/periph/uart_phy.{c,h}` — GP6/GP7/GP8 state machine with
      safe off-then-on transitions.
- [x] RPBP UART subsystem (subsys = 6): opcodes `SET_PHY (0x00)` and
      `GET_PHY (0x01)`; spec at `docs/protocol/subsystems/uart.md`.
- [x] Capability features advertise `"uart.ttl"`, `"uart.rs232"`,
      `"uart.rs485"`; each CDC is its own `buses.uart` entry with
      `phy` and an optional `de_pin` field.

### M4e — Driverless phy selection via dedicated CDC-ACMs

- [x] Composite expanded to 4 CDC-ACMs: one per UART0 phy
      (RS232 / RS485 / TTL) plus the PIO-UART. Host-facing strings
      carry the phy name so the COM ports are self-documenting.
- [x] `cdc_glue.c` watches CDC DTR transitions and auto-switches the
      UART0 phy-bank transceiver on DTR=1. Last DTR-asserted CDC wins
      mutual exclusion on UART0 HW; inactive UART0 CDCs have their
      host writes silently drained so the USB pipe does not stall.
- [x] RPBP `SET_PHY` is retained as the explicit-override path for
      terminals that do not toggle DTR and for CI scripts.
- [x] udev rules publish stable per-phy symlinks: `/dev/rpbridge-
      uart1-rs232`, `rpbridge-uart1-rs485`, `rpbridge-uart1-ttl`,
      `rpbridge-uart2`.
- [x] MS OS 2.0 descriptor set updated — vendor interface numbers
      shifted to 8 (RPBP) and 9 (gs_usb).

### M4d — RS485 DE timing

- [x] **Software DE** (current): `uart_hw` asserts GP6 HIGH before
      any TX burst and releases it once the UART BUSY flag clears,
      driven by `uart_hw_tick()` from the main loop. Correct at all
      supported baud rates (≤ 3 Mbaud) with a tick period ≤ 5 ms.
- [ ] **PIO-precision DE** (future): `src/pio/rs485_de.pio` co-
      clocked with UART0 TX, configurable guard interval. Needed
      only if a scope-level audit shows unacceptable DE edges.

## M5 — CAN over PIO (can2040)

- [ ] `src/pio/can2040_glue.{c,h}` — C ↔ can2040 callback bridge
- [ ] `src/usb/gs_usb_glue.c` — gs_usb-compatible frame translation
      on the second vendor interface

## M6 — Second core + streaming

- [ ] `src/core1/core1_main.{c,h}` — I/O scheduler
- [ ] `src/core1/ipc_queues.{c,h}` — lock-free SPSC rings
- [ ] RPBP stream channels wired to ADC / logic-capture producers

## M7 — Windows binding

- [x] `src/usb/ms_os_20.c` — BOS + MS OS 2.0 descriptor set for
      automatic WinUSB binding on Windows 10 ≥ 1709 (landed with the
      USB-stack-base change; DeviceInterfaceGUIDs are per-vendor-IF).

## M8 — Tests at scale

- [ ] `tests/host/test_sys_dispatch.c` — round-trip through dispatch
      + cmd_sys (needs a stub TX to capture responses)
- [ ] `tests/golden/*` corpus populated — 10+ hand-verified RPBP
      frames consumed by firmware tests and userspace Rust
- [ ] Loopback self-test (`SELFTEST` opcode) exercising every bus on
      the live device

## M9 — Driver-priority mode (dual USB configuration)

Architecture captured in [ADR-0004](../docs/adr/0004-driver-priority-mode.md).
Endpoint / configuration budget tracked in
[`docs/firmware/resource-budget.md`](../docs/firmware/resource-budget.md).

**Goal:** when the `rpbridge` Linux kernel driver is loaded, the
device re-enumerates to a driver-native USB configuration (vendor-
only interfaces, no CDC-ACMs). When the driver is not loaded, the
device stays in the Compat configuration and every inbox USB class
driver binds normally.

Depends on M2 (I²C) + M3 (SPI/GPIO/PWM/ADC) + M5 (CAN) having
landed their RPBP stream-channel definitions — we only know which
channels to carry over the vendor pipe once the subsystems are up.

### M9a — descriptor infrastructure

- [ ] Add `bNumConfigurations = 2` to `desc_device` and a second
      configuration descriptor block in `usb_descriptors.c`.
- [ ] Add `iConfiguration` strings ("Compat", "Native").
- [ ] `tud_descriptor_configuration_cb(index)` returns the right
      descriptor array for the requested index.
- [ ] Verify `tud_cdc_n_ready()` degrades cleanly in Config 2
      (CDC FIFOs are never enumerated, their handlers are no-ops).

### M9b — per-config behaviour

- [ ] Emit an `EVENT` on channel 1 when the active configuration
      changes (host-side audit log).
- [ ] `cdc_glue.c` early-exits cheaply when `tud_ready()` returns
      false — no wasted cycles in driver-native mode.
- [ ] RPBP capability map reports `"mode": "compat" | "native"` and
      the mapping between stream-channel numbers and logical
      functions (UART1 RS232, UART1 RS485, UART1 TTL, UART2 TTL,
      I²C #0, I²C #1, SPI #0, SPI #1, GPIO bank #0, PWM chip #0,
      ADC #0).

### M9c — kernel-driver wiring

Implemented in `driver/src/core_driver.rs`:

- [ ] On `probe()` detect we are on Config 1 (CDC+Vendor) and issue
      `usb_driver_set_configuration(dev, 2)` to hop to Native.
- [ ] Handle re-probe on Config 2 (vendor-only) idempotently.
- [ ] On `disconnect()` let the host-side `usb_reset_configuration()`
      restore Config 1 automatically at next `open()`.
- [ ] Gate aux children on the subsystems advertised in the
      capability map.

### M9d — native tty layer

- [ ] `tty_driver` that backs `/dev/ttyRPB-rs232`,
      `/dev/ttyRPB-rs485`, `/dev/ttyRPB-ttl`, `/dev/ttyRPB-pio`
      over RPBP stream channels. Full termios support; the driver
      forwards SetLineCoding-equivalent RPBP commands to the
      firmware.
- [ ] udev rules list both sets of symlinks — `rpbridge-uart1-*`
      (Config 1) and `ttyRPB-*` (Config 2) — so scripts that were
      written against Compat mode keep working after the driver
      loads.

### M9e — integration tests

- [ ] umockdev recording of the Compat → Native config switch,
      replayable in CI.
- [ ] KUnit test that asserts re-binding lands on Config 2.
- [ ] End-to-end test on real hardware: unplug, plug with driver
      loaded, observe `/dev/ttyRPB*` appearing within 1 s; unload
      driver, replug, observe `/dev/rpbridge-uart1-*` appearing.
