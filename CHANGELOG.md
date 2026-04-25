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
