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

- [ ] `src/usb/cdc_glue.c` — route CDC RX/TX to the selected UART phy
- [ ] `src/periph/uart_hw.{c,h}`
- [ ] `src/pio/rs485_de.pio` — precision DE assert/deassert

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
