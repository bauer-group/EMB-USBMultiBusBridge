# ADR-0007 — FFI bindings as the interim Rust-for-Linux transport

**Status:** Superseded — 2026-04-26 by [ADR-0003 amendment](0003-language-choice.md#amendment--2026-04-26--driver-reverted-to-c)
**Original status:** Accepted — 2026-04-24
**Complements:** [ADR-0003 — Language choice — Rust in kernel and userspace](0003-language-choice.md)

> **Superseded note (2026-04-26).** The Rust-for-Linux driver
> direction this ADR scaffolded was abandoned in favour of a C
> rewrite — see ADR-0003 #Amendment for the full reasoning. The
> historical decision record below is preserved verbatim because it
> documents the reasoning that was operative at the time of the
> attempt; the FFI-binding strategy described here no longer
> reflects the production driver, which is plain C against the
> stable kernel-module ABI (`driver/src/main.c` + per-subsystem
> `.c` files; no FFI shim layer).

## Context

ADR-0003 committed the Linux kernel driver to Rust-only. At commit
time the assumption was that Rust-for-Linux (R4L) would ship safe
wrappers for every subsystem registration API (`kernel::usb::bulk_msg`,
`kernel::i2c::Adapter`, `kernel::spi::Controller`, `kernel::gpio::Chip`,
`kernel::pwm::Chip`, `kernel::iio::Device`, `kernel::auxiliary::Device`)
in time for the RPBridge driver's M10 delivery.

Reality as of Linux 6.18 (Feb/Mar 2026):

- `kernel::usb::Driver` / `probe` / `disconnect` / match tables: **landed**.
- `usb_bulk_msg`, URB submit/anchor, endpoint descriptor walking: **not upstream**.
- `kernel::sync::CondVar::wait_timeout`: **not upstream**.
- All five subsystem registration APIs (gpio_chip, i2c_adapter,
  spi_controller, pwm_chip, iio_device): **not upstream**.
- `kernel::auxiliary::Device`: **not upstream**.

Estimated time-to-upstream for the blocking bindings: Q3–Q4 2026
minimum, very possibly longer. Waiting would leave the driver
non-functional for >6 months after the firmware is feature-complete.

## Decision

Maintain the Rust-only commitment from ADR-0003 and unblock delivery
by writing the missing abstractions ourselves, in a sandboxed FFI
layer at `driver/src/ffi/`. Every kernel C API we consume is paired
with:

1. An `extern "C"` declaration in `ffi/bindings.rs`, pinned to Linux
   6.13 LTS (the supported baseline).
2. A tiny C shim in `ffi/shims.c` that exposes kernel inline helpers
   and struct-field accessors as real symbols, plus allocates typed
   kernel objects and plumbs Rust callbacks through trampolines.
3. A safe Rust wrapper (`ffi/usb.rs`, `ffi/gpio.rs`, `ffi/i2c.rs`,
   `ffi/spi.rs`, `ffi/pwm.rs`, `ffi/iio.rs`, `ffi/wait.rs`) that
   encapsulates every `unsafe` call in a narrow typed API. Each
   `unsafe` block carries a `// SAFETY:` comment documenting the
   invariant the caller relies on.

No unsafe code lives outside `driver/src/ffi/`. The subsystem
adapters (`driver/src/{gpio,i2c,spi,pwm,iio}.rs`) remain fully safe
Rust and are exhaustively tested via MockRoundTrip against
KUnit-for-Rust.

## Rationale

**Why not C?** Memory safety is the stated motivation behind
ADR-0003. A parallel C implementation would duplicate maintenance
and forfeit the safety guarantees on a substantial surface (five
subsystems × USB transport).

**Why not wait for R4L upstream?** Schedule risk. The user-facing
promise of this dev product is "USB bridge with Linux-native
subsystem integration"; that promise cannot be kept on a "maybe next
year" binding timeline. Every FFI binding in `ffi/` has a
deprecation plan tied to its upstream replacement.

**Why not userspace (libusb)?** Breaks the "SoC-like" integration
story — `/dev/i2c-N`, `/dev/spidev*`, `/dev/gpiochip*`, etc. would
not exist. Userspace is not a credible substitute for kernel-level
subsystem exposure.

**Why a separate C shim and not pure-Rust FFI?** Two reasons:
- Several kernel APIs are static-inline helpers (`usb_sndbulkpipe`,
  `list_first_entry_or_null`) that have no exported symbol. A C shim
  gives them one.
- Kernel struct layouts (`usb_interface.cur_altsetting.endpoint[i]`,
  `i2c_msg.flags`, `spi_transfer.tx_buf`) are version-dependent. A
  C shim with `_Static_assert` guards localises any breakage to one
  file.

**Precedent.** The same pattern is in production use by other R4L
drivers during the current transition:

- **Nova GPU** (WIP): raw DRM bindings via FFI while `kernel::drm`
  matures upstream.
- **Binder-rust** (in-tree): uses raw `bindings::` for many APIs
  not yet wrapped safely.
- **Block-rust** test drivers: bindings-direct during the blk-mq
  abstraction's early iterations.

We are not pioneering the pattern; we are adopting it.

## Consequences

Positive:

- **Driver is functional now.** GPIO / I²C / SPI / PWM / IIO
  subsystems all expose their standard `/dev/*` and `/sys/class/*`
  interfaces. libgpiod, i2c-tools, spi-pipe, pwm-utils, iio-utils
  all work unchanged.
- **All of the code remains Rust.** ADR-0003 honoured. The `ffi/`
  module is a thin interop layer whose safe surface is the only
  thing the rest of the driver sees.
- **Excellent test coverage.** 85+ KUnit-for-Rust cases (wire +
  Client) via MockRoundTrip. Every opcode is byte-verified.
- **Clear upstream-replacement path.** Each `ffi/<module>.rs` file
  matches the expected upstream API shape. When the binding lands,
  that file is deleted and call sites rewire — a mechanical change.

Negative:

- **Unsafe surface.** ~350 LoC of `unsafe` blocks (each with
  SAFETY:) plus ~500 LoC of C shim code. This is the cost of
  bypassing the upstream binding timeline and must be reviewed
  accordingly.
- **Kernel-version coupling.** The shim's struct accesses are pinned
  to 6.13 LTS. Breaking changes in 6.x struct layout require a shim
  update. Mitigated by `_Static_assert` on size constants and a
  nightly-mainline CI workflow.
- **auxiliary_bus refactor deferred.** First cut registers subsystems
  directly as children of the USB interface's device (works,
  functionally correct, passes the e2e checklist). ADR-0002's
  auxiliary_bus pattern becomes a pre-upstream-submission refactor
  (M12). Does not block functional delivery.

## Upstream-replacement plan

When R4L lands a safe wrapper for a given API, the corresponding
`ffi/<module>.rs` is deleted and call sites rewire to
`kernel::<module>`. Order of most-likely arrival (based on what's on
the R4L roadmap):

1. `kernel::usb::bulk_msg` (Collabora actively patching).
2. `kernel::sync::CondVar::wait_timeout`.
3. `kernel::auxiliary::Device`.
4. `kernel::gpio::Chip` + `IrqChip`.
5. `kernel::i2c::Adapter`.
6. `kernel::spi::Controller`.
7. `kernel::pwm::Chip`.
8. `kernel::iio::Device` + triggered buffer.

Each migration is a separate PR, reviewed independently. The driver
stays functional throughout.

## References

- [ADR-0003 — Language choice](0003-language-choice.md)
- `driver/src/ffi/` — the whole interop layer this ADR authorises.
- `driver/TODO.md` — M11 + M12 tracking.
- https://lore.kernel.org/rust-for-linux/ — the list where the
  eventual safe bindings will post.
