# ADR-0002 — Linux driver bus pattern

**Status:** Accepted — 2026-04-22

## Context

The RPBridge exposes several subsystems (GPIO, I²C, SPI, PWM, IIO
ADC) behind a single USB interface. The kernel driver has to spawn
one child device per subsystem and bind each to the right subsystem
core (`gpio_chip`, `i2c_adapter`, `spi_controller`, `pwm_chip`,
`iio_dev`).

Two canonical patterns exist in-tree:

1. **MFD + `platform_device`.** Old pattern; used by `dln2`,
   `cros_ec`, `stm32-timers`, and many others. Works, but is
   semantically awkward — a USB-attached peripheral is not a platform
   device — and pulls in a lot of DT/ACPI boilerplate for children
   that should not need any.
2. **`auxiliary_bus`** (mainline since 5.11). Explicitly designed for
   "one parent, many-subsystem children" where the children are not
   independently discoverable. Used by mlx5, ice/irdma, i3c, nvmem
   composites, and is the pattern the networking maintainers nudge
   new drivers toward.

## Decision

Use **`auxiliary_bus`**. The RPBridge core driver matches the USB
interface, parses the CBOR capability map, and spawns one
`auxiliary_device` per advertised subsystem. Each child has its own
`auxiliary_driver` (compiled into the same `rpbridge.ko` module in
our tree; separable later).

## Consequences

Positive:

- Lighter-weight children (no DT/ACPI code paths).
- Matches the mainline-accepted pattern for out-of-tree → upstream
  submission.
- Clean teardown semantics on unplug: the aux_device API frees us
  from manually tracking child lifetimes.

Negative:

- Bumps the minimum supported kernel to 5.11. No practical concern —
  all distributions we target (Ubuntu 22.04+, Debian 12+, Fedora 40+)
  ship newer kernels, and we already require 6.13+ for Rust-for-Linux.

## Supersedes

Supersedes the original architecture draft's "Treiberfamilie nach
MFD-Vorbild" section. The MFD posture predates the decision to adopt
`auxiliary_bus`; this ADR is the authoritative record going forward.
