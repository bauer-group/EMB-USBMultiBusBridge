# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Four components version independently: **protocol** (RPBP), **firmware**,
**driver**, **userspace** — the top-level release tag pins compatible
combinations.

## [Unreleased]

### Added

- Repository scaffolding: firmware (pico-sdk pinned to `2.1.1`), Linux C
  driver skeleton (auxiliary_bus-based), Rust userspace workspace, mkdocs
  documentation site, CI workflow scaffolds.
- Initial ADR set (protocol choice, driver bus choice, language split).
- `.gitattributes`, `.editorconfig`, `.gitmodules`, `CONTRIBUTING.md`,
  `SECURITY.md` per BAUER GROUP standards.
- **ADR-0006**: UART via CDC-ACM only, CAN via gs_usb only. Locks in the
  driver-optional coexistence model from ADR-0005 as a scope decision:
  no `tty_driver` in rpbridge.ko, no RPBP CAN subsystem.
- **Firmware M10a**: `GS_CAN_FEATURE_HW_TIMESTAMP` in the gs_usb
  translator. Every gs_host_frame carries a 4-byte LE µs trailer when
  the host opts in via `GS_CAN_MODE_HW_TIMESTAMP`.
  `GS_USB_BREQ_TIMESTAMP` IN request wired to `time_us_64()` for host
  clock anchoring. Capability flag `"can.hw-timestamp"` added.
- **Firmware M10b**: `docs/protocol/hardware-canfd.md` documenting
  CAN-FD as a hardware gate (can2040 is Classic-CAN only). Compile-
  time `RPBRIDGE_HAVE_CANFD` guard in gs_usb_glue.c hard-fails any
  FD build without FD-capable silicon.
- **Driver M10c**: `src/transport.rs` with `Sequencer` and
  `CompletionTable`. `core_driver::dispatch_rx_frame` routes
  CmdResponse / Error / Event / StreamData.
- **Driver M10d**: `core_driver::spawn_children` walks the CBOR
  capability map and instantiates aux_device children, skipping UART
  and CAN per ADR-0006.
- **Driver M10e..M10i**: per-subsystem `Client<T: RoundTrip>` types
  for GPIO / I²C / SPI / PWM / IIO. Every kernel-binding hook is
  marked with a `§R4L-*-BINDINGS` comment naming the upstream
  binding each adapter consumes. I²C client coalesces write-restart-
  read into `FLAG_NO_STOP`; SPI client drives `HOLD_CS` from
  `cs_change`; PWM client encodes/decodes `pwm_state` ↔
  `{freq_hz, duty_q16}`; IIO client covers single-shot + streaming
  paths including a `STREAM_DATA` parser.
- **Driver M10j**: three-layer test infrastructure — inline
  `#[cfg(test)]` Rust tests (KUnit-for-Rust path), `tests/kunit/` C
  scaffold for CRC-32C + golden-vector CRC parity,
  `tests/umockdev/` replay script with `expected-children.txt`
  manifest, and `tests/e2e.md` ten-section hardware checklist.
- **Driver M10k**: `driver/TODO.md` tracks per-milestone status with
  explicit R4L-binding gates, and the firmware TODO is updated to
  reflect M9c / M9d closure.
- **ADR-0007**: FFI bindings as the interim Rust-for-Linux transport.
  Documents why `driver/src/ffi/` exists (R4L has USB device
  bindings but not bulk_msg / subsystem registration APIs as of
  6.18), what it contains, and the per-API upstream-replacement
  plan that retires each FFI file as the safe binding lands.
- **Driver M11a..M11e**: FFI scaffolding landed. `src/ffi/bindings.rs`
  carries hand-written `extern "C"` declarations pinned to Linux
  6.13 LTS; `src/ffi/shims.c` is a thin C translation unit for
  kernel inline helpers + struct-field accessors + typed object
  allocators; `src/ffi/{usb,wait,auxiliary}.rs` wrap the USB core,
  wait_queue, and parent-device resolution in safe Rust.
- **Driver M11c**: `core_driver::round_trip` is now a real
  synchronous bulk-pair transfer (send CMD_REQUEST on OUT, block-
  read response on IN, validate, correlate seq). `probe()` does
  the full session: endpoint discovery, HELLO handshake,
  `CMD_REQUEST{GET_CAPABILITIES}`, `spawn_children`.
- **Driver M11f..M11j**: all five subsystems register real kernel
  objects. GPIO via `gpio_chip` → `/dev/gpiochipN`. I²C via
  `i2c_adapter` → `/dev/i2c-N`. SPI via `spi_controller` +
  `spidev` → `/dev/spidevX.Y`. PWM via `pwm_chip` →
  `/sys/class/pwm/pwmchipN/`. IIO (single-shot) via `iio_device`
  → `/sys/bus/iio/devices/iio:deviceN/`. IIO triggered-buffer
  streaming is tracked as M12 (needs an async event kthread).
- **Driver M11k+l**: 85+ KUnit-for-Rust test cases total.
  `transport::MockRoundTrip` lets every subsystem Client be
  exercised end-to-end without USB hardware; per-subsystem
  `client_tests` modules cover every opcode byte-for-byte plus
  errno propagation (EBUSY, ETIMEDOUT, ENODEV, ENOTSUP).
- **ADR-0008**: Multi-board variants via board-header + feature-flag
  refactor. Establishes that every board-specific value lives in
  `firmware/boards/<name>.h` and every source file consumes it via
  `RPBRIDGE_*` macros; new variants are a header file + a CMake
  matrix entry, no source edits.
- **Firmware M11.1..M11.8**: full multi-board refactor across the
  firmware tree. `pin_broker`, `gpio`, `pwm`, `i2c_hw`, `spi`,
  `uart_phy`, `cdc_glue`, `adc`, and `capabilities.c` all now
  consume `RPBRIDGE_*_PIN` / `RPBRIDGE_USER_*_PIN_LIST` /
  `RPBRIDGE_HAVE_*` from the active board header. Subsystems whose
  hardware is absent (`HAVE_I2C1=0`, `HAVE_RS232_PHY=0`, etc.)
  collapse to nothing via `#if` guards and disappear from the
  capability map. The Linux driver picks up these omissions
  automatically because aux_device spawning is capability-driven.
- **Firmware: rpbridge_pico2 board target**. Bare Raspberry Pi
  Pico 2 (RP2350A QFN-60, 4 MB external W25Q32JV) is now a
  first-class build target alongside the rev-B PCB. PIO-UART
  remapped to GP6/GP7 (rev-B has it on GP33/GP36); I²C #1, SPI #1,
  RS232 and RS485 phys disabled; user GPIO/PWM whitelist on GP8..15.
  Build with `cmake -DPICO_BOARD=rpbridge_pico2`. Output:
  `rpbridge-pico2.uf2`. Lets users get started for $5 without
  custom hardware.
- **Build infrastructure**: `firmware/CMakeLists.txt` validates
  `PICO_BOARD` against `RPBRIDGE_SUPPORTED_BOARDS`. Output binary
  renamed per variant. `firmware/build-all-variants.sh` builds
  every variant in one shell loop.
  `.github/workflows/firmware.yml` matrix axis includes both
  variants, so every push exercises both targets in 4 jobs.
- **Firmware M12 — PIO2 applets: WS2812 + DMX512.** Two new
  subsystems land together on the previously unused PIO2 block:
  - **WS2812 / NeoPixel addressable LED strips** as RPBP subsystem
    `0x07`. New `src/pio/ws2812.pio` (4-instr NRZ encoder, SM clock
    8 MHz, 1.25 µs/bit), `src/peripherals/led_strip.{c,h}` driver
    (256-pixel max framebuffer, GRB / RGB / GRBW formats, 80 µs
    latch hold), and `src/commands/cmd_led.{c,h}` dispatcher with
    opcodes `GET_INFO / CONFIGURE / WRITE / CLEAR`. Wire format
    documented in `docs/protocol/subsystems/led.md`. Pin:
    `RPBRIDGE_LED_STRIP_PIN` (GP43 rev-B, GP14 pico2). Feature flag
    `led.ws2812`.
  - **DMX512-A transmitter** as RPBP subsystem `0x08`. New
    `src/pio/dmx_tx.pio` (8N2 UART at 250 kbaud), and
    `src/peripherals/dmx.{c,h}` driver — BREAK + MAB are generated
    by the C side via `pio_sm_set_pins_with_mask` while the SM is
    paused, leaving timing host-tunable. 513-byte universe buffer
    with tunable BREAK/MAB; `dmx_tick()` runs the ~40 Hz frame
    cadence from the main loop. `src/commands/cmd_dmx.{c,h}`
    dispatcher with opcodes `GET_INFO / SET_SLOTS / START / STOP /
    SET_BREAK`. Wire format in `docs/protocol/subsystems/dmx.md`.
    Pin: `RPBRIDGE_DMX_TX_PIN` (GP44 rev-B, GP15 pico2; user wires
    their own RS-485 transceiver). Feature flag `dmx.tx`.
  Both subsystems are gated on `RPBRIDGE_HAVE_LED` /
  `RPBRIDGE_HAVE_DMX` board flags (default ON for both shipping
  variants). PIO2 utilisation after this milestone: 2/4 SMs and
  ~12/32 instr (still room for the planned 1-Wire applet).
- **Driver M12 — wire-only Rust clients for LED + DMX.** New
  `driver/src/led.rs` and `driver/src/dmx.rs` follow the
  established `wire` builders + `Client<T: RoundTrip>` two-layer
  pattern. `LedChild` and `DmxChild` placeholders integrate with
  `core_driver::spawn_children` so the cap-map → child probe flow
  is exercised end-to-end; both call `GET_INFO` and log the
  discovered universe. Layer 3 (kernel binding) is deferred until
  upstream R4L wrappers exist for `led_classdev_multicolor` and a
  DMX char-device — tracked in `driver/TODO.md` §M12. Per-module
  inline tests + `MockRoundTrip` integration tests cover every
  opcode byte-for-byte.
- **Firmware M12.2 — PIO1 + PIO2 expansion: 1-Wire + I²S TX.**
  Two more applets land to maximise the chip's PIO budget. Final
  utilisation: PIO0 sealed for can2040, PIO1 3/4 SMs ~25/32 instr,
  PIO2 3/4 SMs ~17/32 instr (1 SM + 6-15 instr free in each
  non-CAN block for future logic-capture / RS485 precision-DE).
  - **1-Wire master** as RPBP subsystem `0x09`. New
    `src/pio/onewire.pio` (~14 instr; pindirs-as-open-drain
    emulation per Maxim AN126), `src/peripherals/onewire.{c,h}`
    on PIO1 SM 2 with byte-level composition (LSB-first), and
    `src/commands/cmd_onewire.{c,h}` dispatcher with opcodes
    `GET_INFO / RESET / WRITE / READ / TRIPLET`. The TRIPLET
    primitive (one round-trip, returns id_bit + cmp_bit +
    direction-written) is the SEARCH-ROM building block per
    Maxim AN187. Wire format in
    `docs/protocol/subsystems/onewire.md`. Pin:
    `RPBRIDGE_ONEWIRE_PIN` (GP32 rev-B, GP12 pico2). Feature
    flag `onewire.master`.
  - **I²S TX (Philips I²S, 16-bit signed PCM stereo)** as RPBP
    subsystem `0x0A`. New `src/pio/i2s_tx.pio` (8 instr; mirrors
    pico-extras `audio_i2s.pio` BSD-3 pattern), and
    `src/peripherals/i2s.{c,h}` driver on PIO2 SM 2.
    `_Static_assert` enforces `LRCLK_PIN == BCLK_PIN + 1`
    (side-set requirement). Sample rate 8 kHz..96 kHz; the SM
    clock is 64 × sample_rate so 32 BCLKs per L/R frame.
    `src/commands/cmd_i2s.{c,h}` dispatcher with opcodes
    `GET_INFO / CONFIGURE / START / STOP / WRITE`. v1 scope is
    buffer-playback (WRITE blocks until the FIFO accepts);
    continuous streaming via STREAM channel is a follow-up.
    Wire format in `docs/protocol/subsystems/i2s.md`. Pins:
    `RPBRIDGE_I2S_BCLK_PIN` / `_LRCLK_PIN` / `_DOUT_PIN`
    (GP41/42/45 rev-B, GP8/9/13 pico2). Feature flag `i2s.tx`.
  Both subsystems are gated on `RPBRIDGE_HAVE_ONEWIRE` /
  `RPBRIDGE_HAVE_I2S` board flags (default ON for both shipping
  variants). Subsystem IDs `0x09` ONEWIRE + `0x0A` I²S.
- **Driver M12.2 — wire-only clients for 1-Wire + I²S.** New
  `driver/src/onewire.rs` and `driver/src/i2s.rs`, same shape as
  LED/DMX (wire builders + `Client<T: RoundTrip>`).
  `OnewireChild::probe()` and `I2sChild::probe()` integrate with
  `spawn_children`. The 1-Wire `Client::triplet()` returns a
  decoded `TripletResult { id_bit, cmp_bit, dir }` so a future
  kernel w1_master adapter can drive SEARCH ROM with one
  round-trip per ROM bit. The I²S `Client::write()` rejects
  unaligned (non-multiple-of-4) PCM payloads before hitting the
  transport. Inline + `MockRoundTrip` integration tests cover
  every opcode and decode all four (id_bit, cmp_bit) outcomes for
  the SEARCH ROM triplet.

### Hardened (forward-compat)

- **Status code mapping**: `Status::try_from` in the driver now
  gracefully degrades unknown status bytes (16..=255) to
  `Status::EIo` instead of raising `EPROTO`. RPBP v1 §16 promises
  MINOR bumps may add status codes; an old driver against future
  firmware now surfaces such codes as a generic `EIO` to userland
  rather than misclassifying them as protocol corruption. Two new
  inline tests pin the contract.
- **USB VID/PID pre-production marking**: the pid.codes testing-
  range PID `0xFFFE` is now flagged with explicit warnings in
  `usb_descriptors.c` and `core_driver.rs`, plus a dedicated
  [`docs/usb-vid-pid.md`](docs/usb-vid-pid.md) that documents the
  allocation strategy (one VID/PID per device, subsystems via
  Interface-Class), the pre-production gates, and why we chose
  pid.codes over USB-IF for v1. The release-blocking checklist
  lives in `firmware/TODO.md` → "Pre-production gates".

### Fixed

- **Fat-pointer cookie bug across all 5 FFI modules.** Every
  `ffi/{gpio,i2c,spi,pwm,iio}.rs` stored `Box::into_raw(Box<dyn
  Trait>)` as a thin `*mut c_void`, discarding the trait object's
  vtable. First callback from the kernel would have dereferenced
  a garbage vtable and Oops'd the whole subsystem. Replaced with a
  double-Box pattern (`Box<Box<dyn Trait>>`) so the outer pointer
  is thin and safe to round-trip through `c_void`, while the inner
  box retains the fat trait pointer with its vtable intact.
- **IIO channels array missing.** `iio_device_register()` rejects
  `num_channels > 0` with a NULL `channels` array. Populated a
  static `rpbridge_iio_channels[]` in the shim with the canonical
  rev-B map (4 voltage + 1 temperature) and clamped
  `num_channels` at allocation time.
- **IIO triggered-buffer stub would crash.** The call
  `iio_triggered_buffer_setup(indio, NULL, NULL, NULL)` passes
  NULL pollfunc handlers, which the IIO core dereferences.
  Stubbed the setup / teardown / push helpers to return `-ENOSYS`
  / no-op; async streaming is now explicitly an M12 deliverable.
- **SPI shim against 6.18 API.** Replaced `spi_alloc_master` with
  `spi_alloc_host` (renamed in 6.17), replaced direct
  `spi_device->chip_select` access with `spi_get_chipselect()`,
  and added the missing `spi_finalize_current_message` call so
  the second and subsequent SPI messages actually get scheduled.
- **PWM shim against 6.18 API.** `pwm_chip` has been opaque since
  6.10; replaced the `kzalloc + container_of` pattern with
  `pwmchip_alloc(parent, npwm, sizeof(void*))` + `pwmchip_priv()`
  cookie access + `pwmchip_put()` on teardown.
- **`new_mutex!` import.** Added explicit `use kernel::sync::new_mutex;`
  in core_driver; the macro is not reliably in the prelude on 6.18.
- **Interface pointer cast.** Replaced the `core::mem::transmute`
  on `&kernel::usb::Interface` with an explicit pointer cast chain
  that documents the transparent-layout assumption in one place.

### Changed

- ADR-0004 (dual USB configuration) marked as Superseded by ADR-0005
  in the ADR index.
- **Kernel baseline bumped from 6.13 LTS to 6.18** in
  `driver/kernel-baseline/SUPPORT.md`. 6.13 was aspirational —
  `kernel::usb::Driver` only exists from 6.18 (Daniel Almeida /
  Collabora), so the driver can't actually compile against
  anything older. 6.13..6.17 now listed as unsupported.
- Driver status reflected honestly in `driver/TODO.md` — code
  structure complete + post-audit bug sweep applied, but
  `make`/KUnit/umockdev/e2e-on-hardware all remain "not yet run".

### Notes

- This changelog starts when the repository transitions from design-draft
  stage to implementation stage. The design drafts that led to this
  structure are preserved under [`docs/drafts/`](docs/drafts/).
