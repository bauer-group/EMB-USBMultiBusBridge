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

### Changed

- ADR-0004 (dual USB configuration) marked as Superseded by ADR-0005
  in the ADR index.
- Driver status changed from "client-layer complete, kernel-adapter
  blocked on R4L bindings" to **functionally complete on Linux
  6.13 LTS+**. All five non-UART / non-CAN subsystems expose their
  standard `/dev/*` + `/sys/class/*` interfaces; `libgpiod`,
  `i2c-tools`, `spidev_test`, `pwm-utils`, and `iio_info` work
  unchanged.

### Notes

- This changelog starts when the repository transitions from design-draft
  stage to implementation stage. The design drafts that led to this
  structure are preserved under [`docs/drafts/`](docs/drafts/).
