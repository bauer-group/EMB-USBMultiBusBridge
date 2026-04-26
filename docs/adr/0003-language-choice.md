# ADR-0003 — Language choice

**Status:** Superseded — 2026-04-26 (driver pivoted from Rust to C; see Amendment below)
**Original status:** Accepted — 2026-04-22

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

## Amendment — 2026-04-26 — Driver reverted to C

**Status:** Driver is now C-only. Firmware (C, pico-sdk) and
userspace (Rust) decisions are unchanged.

### Why we reverted

The Rust-for-Linux out-of-tree driver was first attempted to compile
against the kernel that Ubuntu 26.04 LTS ships (Linux 7.0) on
2026-04-26. Two layers of pain surfaced:

1. **Latent bugs from never-validated code.** The driver was written
   against a 6.18 R4L snapshot but `driver/TODO.md` M11d
   ("`make` against 6.18 kernel") was always marked open. First
   compile-pass exposed a `bitflags` crate import that the kernel
   build can't satisfy (no crates.io), a `RoundTrip` impl on
   `Arc<RpbridgeState>` that didn't help generic methods on `T`,
   and several similar "would have been caught the first time
   anyone ran make" issues.
2. **R4L API churn between 6.18 and 7.0.** `kernel::usb::Driver`
   shed its `type Data` associated type, the `#[vtable]` macro was
   removed from this trait, `suspend`/`resume` moved into a
   separate `DriverPM` trait, `probe()` now returns
   `impl PinInit<Self, Error>` instead of `Result<Arc<...>>`,
   `KString` was removed in favour of `CString`, and every
   `new_mutex!` site has to live inside a `pin_init!` block
   because the macro now produces an `impl PinInit<Lock<T>>`
   rather than a `Lock<T>` value.

After fixing ~50 % of the resulting 200+ errors (commits 0ae313f,
ba2d81b, cb1eafa, 7b905ae) the remaining cluster — the cap.rs
`KString` migration, the four ffi/* subsystem-binding shims, and
the cascading client-trait-bound errors — required reading the
exact 7.0 R4L source to drive correctly, which was not available
to the in-container build (sandbox blocked external clones, the
shipped `linux-lib-rust-7.0.0-14-generic` package only carries
rmeta files, not sources).

### Why C is the right call for THIS driver

- **Stable kernel-module ABI.** Linux's hard guarantee since 2.6:
  a C module compiled against last year's headers binds today's
  kernel. R4L makes no such promise — and won't, until the API
  surface stabilises (no concrete timeline).
- **Distro reach.** Ubuntu 26.04 LTS ships kernel 7.0; within a
  year 8.x lands. A C driver compiles unchanged across that
  range. An out-of-tree R4L module rebases its trait impls on
  every major.
- **Footgun parity.** Memory safety in C is not free — but the
  patterns are well understood (kref for refcounting, scoped
  locking, defensive parameter validation, never-trust-the-bus
  for length fields). The new C driver applies all four
  uniformly; review-time verifiable rather than type-system
  enforced.
- **Triage cost.** When something goes wrong on a customer
  machine (different kernel, different distro), debugging C is
  the well-trodden path. R4L diagnostic familiarity inside the
  community is far smaller.

### What we kept Rust for

- **Userspace.** `userspace/rpbridge`, `rpbridge-protocol`,
  `rpbridge-ctl`, `rpbridge-bench` stay in Rust. The userspace
  Rust toolchain is rustup-stable, not kernel-paired; no
  ABI-instability issue.
- **Future revisit.** When R4L stabilises (probably a year past
  the C driver shipping), the question reopens. The earlier
  Rust scaffold lives on the `archive/rust-driver` git branch
  for exactly that comparison.

### Implementation reset

The driver was reset to a clean C scaffold (commit `b9f9392`):

- `driver/src/main.c` — USB driver entry, kref-based lifetime
- `driver/src/rpbp.h` + `rpbp_framing.[ch]` + `rpbp_crc32c.[ch]`
- `driver/src/transport.[ch]` — sync round-trip
- `driver/src/cap.[ch]` — CBOR capability parser
- `driver/src/session.[ch]` — HELLO + capability fetch glue

`kernel-baseline/SUPPORT.md` updated: minimum 5.15 LTS
(auxiliary_bus baseline), primary CI target 7.0 LTS
(Ubuntu 26.04 HWE), expected-OK on 8.x.
