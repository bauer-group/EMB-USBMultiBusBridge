# Why RPBridge

Linux developers regularly need access to low-speed peripheral buses
on workstations, laptops and SBCs that do not expose them natively.
The alternatives on the market are either

- closed-source vendor dongles with bespoke APIs, requiring their own
  library + daemon on every machine,
- single-bus adapters (CP2102 for UART, FT232H for one bus, MCP2221
  for I²C or GPIO…),
- expensive industrial bridges that package everything but lock users
  into proprietary drivers.

RPBridge unifies **seven bus classes on one open-source device**. Each
one appears as a standard Linux subsystem entry — `/dev/i2c-N`,
`/dev/spidevN.M`, `/dev/gpiochipN`, `/dev/ttyACM*`, `can0` — which
means every existing tool, script, and service keeps working
unmodified:

- `i2c-tools` (`i2cdetect`, `i2cget`, `i2cset`, `i2ctransfer`)
- `libgpiod 2.x` (`gpioset`, `gpioget`, `gpiomon`) and anything
  linking `libgpiod`
- `can-utils` (`cansend`, `candump`, `cangen`, `cansequence`) + any
  SocketCAN-based software (Wireshark CAN dissector, SavvyCAN,
  python-can)
- `flashrom` and `spidev_test` on SPI flash chips
- `iio_info` / `iio_readdev` for the ADC — and through `libiio`,
  Grafana and friends
- any terminal that talks to `/dev/ttyACM*` (PuTTY, minicom, picocom,
  tio, Arduino IDE)

## Why RP2354B

- 2× Cortex-M33 @ 150 MHz with FPU and DSP, 520 KB SRAM.
- **12 PIO state machines** across 3 PIO blocks — enough to run CAN
  2.0B in software (via [can2040](https://github.com/KevinOConnor/can2040))
  while still leaving room for PIO-based UARTs, 1-Wire, WS2812, and
  custom applets.
- 2 MB stacked QSPI flash integrated in the QFN-80 package — one less
  external IC on the board.
- USB 2.0 Full-Speed (12 Mbit/s) native. We lose high-speed bandwidth
  relative to chips with HS PHYs, but USB-FS is enough to share seven
  low-speed buses with headroom.
- Excellent tooling (pico-sdk, picotool, TinyUSB) and a well-supported
  open ecosystem.

## Why Rust in userspace and in the kernel

For the **kernel driver**: starting with Linux 6.13, Rust-for-Linux
provides stable enough USB and `auxiliary_bus` bindings to build an
entire MFD-style driver without touching C. One implementation is
easier to audit, test and refactor than a parallel C/Rust pair, so we
committed to a Rust-only kernel module.

For **userspace**: memory safety, a first-class async ecosystem
(`nusb`, `tokio`), a crate-per-binding model that makes FFI bindings
to Python and .NET straightforward, and a build system that cross-
compiles to ARM SBCs without surprises.

See [ADR-0003](adr/0003-language-choice.md) for the full reasoning.
