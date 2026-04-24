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

### Changed

- ADR-0004 (dual USB configuration) marked as Superseded by ADR-0005
  in the ADR index.

### Notes

- This changelog starts when the repository transitions from design-draft
  stage to implementation stage. The design drafts that led to this
  structure are preserved under [`docs/drafts/`](docs/drafts/).
