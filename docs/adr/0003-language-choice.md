# ADR-0003 — Language choice

**Status:** Accepted — 2026-04-22

## Context

Three target audiences, three language decisions:

1. **Firmware** runs on a Cortex-M33 with 520 KB of SRAM. Vendor SDK
   (pico-sdk) is C-with-CMake. Rust-on-RP2350 is viable (embassy,
   rp-hal) but diverges from the library that the official CAN
   implementation (can2040) and the USB stack (TinyUSB) live in.
2. **Linux kernel driver** can now be written entirely in Rust, via
   Rust-for-Linux. Starting with 6.13, the USB, auxiliary_bus,
   libcrc32c and subsystem-core bindings the driver needs are stable
   enough for a standalone out-of-tree module.
3. **Userspace** has no legacy constraint. Python, Rust, Go, and C++
   all work; bindings ultimately have to exist for each.

## Decision

- **Firmware: C** with pico-sdk 2.2.0. Vendor-native toolchain,
  battle-tested, direct access to can2040 and TinyUSB without glue.
- **Linux kernel driver: Rust only.** No C fallback to maintain in
  parallel. Minimum kernel 6.13.
- **Userspace: Rust as the primary implementation.** Python and .NET
  bindings are generated on top of a stable C ABI (cbindgen / uniffi
  once the API stabilises) rather than re-implemented per language.

The distinction between firmware and kernel-driver language is *not*
dogma — it is a pragmatic choice driven by toolchain ergonomics on
each side. The wire protocol is language-neutral and the Rust and C
implementations of RPBP are tested against a shared golden-vector
corpus.

## Consequences

Positive:

- Kernel code benefits from Rust's memory safety in the most
  security-sensitive layer of the stack.
- Userspace Rust library doubles as the FFI basis for Python and
  .NET bindings — no re-implementation per language.
- The firmware keeps vendor-tool compatibility, which matters for
  debugging, profiling (`picotool`, RTT, SEGGER Ozone) and bring-up.

Negative:

- A Rust kernel module cannot be built on older distributions without
  CONFIG_RUST=y. Users on Ubuntu 22.04 / RHEL 9 / Debian 12 need to
  upgrade the kernel via HWE/kernel-ml or wait for the next LTS.
- Contributors need both C and Rust skill to move between layers.
  Judged acceptable given the skill overlap in the embedded/systems
  community.

## Supersedes

Supersedes the implicit C-primary posture of the original
`rpbridge-architektur.md` §8 and locks in the Rust-only driver
direction explicitly stated by the maintainer during planning.
