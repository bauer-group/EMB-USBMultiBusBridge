# Firmware — implementation roadmap

The scaffold in this tree compiles against pico-sdk 2.2.0 and brings up
USB enumeration with 3× CDC-ACM + 2× vendor interfaces. The modules
below are **not yet wired** — add them here as they land, keep the
CMakeLists source list in sync.

Priorities follow the milestones in `docs/drafts/rpbridge-architektur.md`
§11.

## M1 — vendor-transport echo

- [ ] `src/usb/vendor_class.c` — TinyUSB vendor callbacks (RPBP RX/TX)
- [ ] `src/rpbp/rpbp_dispatch.{c,h}` — request demultiplexer
- [ ] `src/cmd/cmd_sys.c` — `OP_ECHO`, `OP_GET_CAPS`, `OP_REBOOT_BOOTSEL`
- [ ] `src/cap/capabilities.{c,h}` — CBOR capability serializer
  (use `tinycbor` as a submodule, pinned)
- [ ] `src/usb/ms_os_20.c` — BOS + MS OS 2.0 descriptor set so Windows
  binds WinUSB automatically

## M2 — HW-I²C + pin broker

- [ ] `src/periph/pin_broker.{c,h}`
- [ ] `src/periph/i2c.{c,h}`
- [ ] `src/cmd/cmd_i2c.c`

## M3 — SPI + GPIO + PWM + ADC

- [ ] `src/periph/spi.{c,h}` + `src/cmd/cmd_spi.c`
- [ ] `src/periph/gpio.{c,h}` + `src/cmd/cmd_gpio.c`
- [ ] `src/periph/pwm.{c,h}` + `src/cmd/cmd_pwm.c`
- [ ] `src/periph/adc.{c,h}` + `src/cmd/cmd_adc.c`

## M4 — CDC-ACM bridge to HW-UART / RS485-DE applet

- [ ] `src/usb/cdc_glue.c` — route CDC RX/TX to the selected UART phy
- [ ] `src/periph/uart_hw.{c,h}`
- [ ] `src/pio/rs485_de.pio` + wrapper

## M5 — CAN over PIO (can2040)

- [ ] `src/pio/can2040_glue.{c,h}` — C ↔ can2040 library bridge
- [ ] `src/usb/gs_usb_glue.c` — Geschwister-Schneider gs_usb emulation
  layer on the second vendor interface

## M6 — core 1 scheduler

- [ ] `src/core1/core1_main.{c,h}` — IO scheduler on the second core
- [ ] `src/core1/ipc_queues.{c,h}` — lock-free SPSC rings

## M7 — tests

- [ ] `tests/host/` — CRC / framing / capability unit tests on the
  host toolchain (`-DRPBRIDGE_HOST_TESTS=ON`)
- [ ] Loopback selftest (`OP_SELFTEST`) exercising every bus on the
  live device
