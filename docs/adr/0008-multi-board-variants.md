# ADR-0008 — Multi-board variants via board-header + feature-flag refactor

**Status:** Accepted — 2026-04-25
**Complements:** [ADR-0006 — UART via CDC-ACM only, CAN via gs_usb only](0006-uart-cdc-only-can-gs-usb-only.md)

## Context

The original firmware was hand-targeted at the rev-B PCB built around
the RP2354B (RP2350B die, QFN-80, 48 GPIOs). The board layout, pin
numbers, transceiver presence, and subsystem instance counts were
all baked into source files (`pin_broker.h::PIN_BROKER_NUM_PINS=48`,
`gpio.c` whitelist `{30, 31, 37..45}`, `i2c_hw.c` SDA=GP34/SCL=GP35,
etc.).

The dev product needs a path for users to start without buying or
building a custom PCB. The Raspberry Pi Pico 2 — same RP2350-family
silicon (RP2350A, QFN-60, 30 GPIOs), 4 MB external QSPI flash, $5 —
is the obvious zero-cost entry point. Same compiler, same SDK, same
PIO model, same SRAM, same Cortex-M33; the differences are pin
count, package pinout, and on-board peripherals (LED layout, no
RS232/RS485 transceivers).

A user-installable Pico 2 build also enables: external CI smoke
testing on commodity hardware, bring-up labs without PCB lead time,
and live demos on a board the audience can buy on the way home.

The RP2040 (Pi Pico v1) is **explicitly out of scope**: different
core (M0+ vs M33), half the SRAM (264 KB vs 520 KB), one fewer PIO
block (8 SMs vs 12), tighter can2040 timing margins. A separate ADR
can revisit if a Cost-Down case appears.

## Decision

Refactor the firmware so every board-specific value lives in a
single board header (`firmware/boards/<name>.h`) and every source
file consumes it via `RPBRIDGE_*` macros. Add explicit feature
flags (`RPBRIDGE_HAVE_*`) for subsystems whose presence depends on
the variant, and use `#if` guards in `.c` files so a missing
subsystem compiles to nothing.

Two variants ship in v1:

| Variant | MCU | Package | Pins | I²C #1 | SPI #1 | RS232/RS485 | PIO-UART pins |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `rpbridge_rp2354b` | RP2350B | QFN-80 | 48 | ✓ | ✓ | populated | GP33/GP36 |
| `rpbridge_pico2`   | RP2350A | QFN-60 | 30 | ✗ | ✗ | not populated | GP6/GP7 |

A user picks the variant at configure time:

```sh
cmake -S firmware -B build -DPICO_BOARD=rpbridge_pico2
cmake --build build
# → build/rpbridge-pico2.uf2
```

The output filename includes the variant suffix; a single CI matrix
(2 boards × 2 build types) builds all four artefacts on every push.

## Architecture

### Single source of truth

Every pin number, every instance count, every feature toggle is a
macro in the board header. Source files contain zero hardcoded
GPIO numbers and zero "this exists on rev-B" assumptions. Examples
of the macro families:

- `RPBRIDGE_PIN_BROKER_SLOTS` — 30 or 48
- `RPBRIDGE_UART0_TX_PIN` / `RPBRIDGE_UART0_RX_PIN`
- `RPBRIDGE_PIO_UART_TX_PIN` / `RPBRIDGE_PIO_UART_RX_PIN`
- `RPBRIDGE_I2C0_SDA_PIN` / `RPBRIDGE_I2C0_SCL_PIN` / `_I2C1_*`
- `RPBRIDGE_SPI0_SCK_PIN` / `_MOSI_PIN` / `_MISO_PIN` / `_SPI1_*`
- `RPBRIDGE_CAN_TX_PIN` / `RPBRIDGE_CAN_RX_PIN`
- `RPBRIDGE_RS485_DE_PIN` / `RPBRIDGE_RS485_EN_PIN` / `RPBRIDGE_RS232_SHDN_PIN`
- `RPBRIDGE_ADC_CH0..3_PIN` + `RPBRIDGE_ADC_EXT_CHANNEL_COUNT`
- `RPBRIDGE_USER_GPIO_PIN_LIST` + `RPBRIDGE_USER_GPIO_PIN_COUNT`
- `RPBRIDGE_USER_PWM_PIN_LIST`  + `RPBRIDGE_USER_PWM_PIN_COUNT`
- `RPBRIDGE_BOARD_NAME` (string in capability map + USB descriptor)

### Feature flags

For subsystems that may be absent on a given board:

- `RPBRIDGE_HAVE_PIO_UART` (1/0)
- `RPBRIDGE_HAVE_I2C1` (1/0)
- `RPBRIDGE_HAVE_SPI1` (1/0)
- `RPBRIDGE_HAVE_CAN` (1/0)
- `RPBRIDGE_HAVE_RS232_PHY` (1/0)
- `RPBRIDGE_HAVE_RS485_PHY` (1/0)

`#if RPBRIDGE_HAVE_X` guards in `i2c_hw.c`, `spi.c`, `uart_phy.c`,
`cdc_glue.c`, and the capability emitter ensure absent subsystems
compile to nothing.

### Capability map mirrors the build

The CBOR capability map's bus arrays, feature lists, and pin lists
are all computed at compile time from the same macros. The Linux
driver's `cap.rs` parser already walks variable-length arrays, so
no driver-side changes are needed — `rpbridge.ko` automatically
spawns the correct subset of aux_devices for whichever board it
attaches to.

### Conventions for adding a new board

1. Copy `boards/rpbridge_pico2.h` to `boards/rpbridge_<name>.h`.
2. Override the `RPBRIDGE_*` macros that differ.
3. Add the board name to `RPBRIDGE_SUPPORTED_BOARDS` in
   `firmware/CMakeLists.txt` and to `ALL_VARIANTS` in
   `firmware/build-all-variants.sh`.
4. Add the board to the GitHub Actions matrix axis.
5. Document the board in this ADR's variant table.

No source-file edits required for a new variant unless the board
introduces a peripheral type that doesn't exist yet.

## Consequences

Positive:

- **User onboarding** — anyone with a $5 Pi Pico 2 can flash
  `rpbridge-pico2.uf2` and immediately get the I²C #0, SPI #0, CAN,
  ADC, GPIO/PWM subsystems on their host. RS232/RS485 require user
  wiring of an external transceiver if needed.
- **Demo portability** — a Pi Pico 2 fits in a pocket and works
  identically to the rev-B board for everything that's pinned out.
- **CI coverage** — every commit builds both variants, catching
  board-conditional regressions (e.g. dropping `#if RPBRIDGE_HAVE_X`
  by accident) at PR time instead of in the next bring-up lab.
- **Adding boards is mechanical** — a new variant is one new header
  + three lines in the CI matrix. No source edits.
- **Driver unchanged** — `rpbridge.ko` walks the capability map; if
  the device says "no I²C #1", the driver doesn't try to register
  one.

Negative:

- **Bare-board UX** — Pi Pico 2 enumerates the same 4-CDC composite
  as rev-B because TUSB's `CFG_TUD_CDC` is a compile constant. On a
  bare board the RS232 and RS485 CDCs exist but are effectively
  TTL aliases (the phy switcher refuses to enable absent silicon).
  Acceptable for v1; collapsing to a single CDC on bare boards is
  a follow-up cleanup.
- **PWM RGB LED on Pi Pico 2** — the board has only a single green
  LED on GP25. The firmware drives R/G/B onto the same pin so any
  colour request lights it; brightness/colour distinction is lost.
  A proper "single LED mode" gate is a future cleanup.
- **Macro discipline must be maintained** — every new pin number
  added to the source must be `RPBRIDGE_*_PIN`, not a literal. A
  CI grep-check is on the M12 follow-up list.

## Alternatives considered

- **Multiple top-level firmware projects** — one CMakeLists.txt per
  board. Rejected: triples the maintenance surface and breaks
  source unification; the SDK supports `PICO_BOARD` precisely so
  you don't need this.
- **Runtime board detection** — read GPIO IDs at boot. Rejected:
  pin count itself is compile-time fixed by linker layout, and the
  firmware-image needs to be the right size for the right flash.
- **Userspace wrapper that stitches together two firmware halves** —
  way too clever; debugging would be a nightmare.

## Implementation sequencing

The refactor landed as 8 atomic commits (M11.1 through M11.8 in
`firmware/TODO.md`). Each commit is independently buildable on the
rev-B target so a bisect across the series stays useful:

1. Extend `rpbridge_rp2354b.h` with all peripheral pin macros +
   feature flags (no source change).
2. Migrate `pin_broker` + `adc` to board macros.
3. Migrate `gpio` + `pwm` whitelists.
4. Migrate `i2c` + `spi` to macros + `#if HAVE_X` gates.
5. Migrate `uart_phy` + `cdc_glue` to macros + `HAVE_RS232/485/PIO`
   gates.
6. Make `capabilities.c` fully feature-flag-driven.
7. Add `rpbridge_pico2.h`.
8. Add CMake variant validation + CI matrix + build-all-variants.sh.

## References

- `firmware/boards/rpbridge_rp2354b.h` — reference (max-features) variant.
- `firmware/boards/rpbridge_pico2.h` — Pi Pico 2 variant.
- `firmware/CMakeLists.txt` — `RPBRIDGE_SUPPORTED_BOARDS` validation.
- `firmware/build-all-variants.sh` — multi-build wrapper.
- `.github/workflows/firmware.yml` — CI matrix.
- ADR-0006 — locks the UART-CDC and CAN-gs_usb decisions that this
  refactor inherits unchanged.
