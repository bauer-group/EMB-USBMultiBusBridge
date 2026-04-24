# Driver tests

Three layers, each targeting a different regression class.

| Layer | Runtime | Catches | Needs hardware? |
| --- | --- | --- | --- |
| inline `#[cfg(test)]` | KUnit-for-Rust | Wire-format encoding/parsing, Client logic, completion table, capability parser | no |
| `kunit/` | KUnit on a kernel build with `CONFIG_RUST=y` | Module loading, probe-path wiring, RPBP framing inside the kernel | no (QEMU works) |
| `umockdev/` | umockdev replay + user-space runner | USB-descriptor handling, aux_device spawn, subsystem exposure | no (uses recorded device) |
| `e2e.md` | Real RPBridge + Linux host | End-to-end bus exercises (I²C / SPI / GPIO / PWM / IIO / CAN) | yes |

## Layer 1 — inline Rust tests (KUnit-for-Rust)

Pure-Rust unit tests for the kernel-binding-agnostic layers live as
`#[cfg(test)] mod tests` blocks inside each driver source file:

- `src/rpbp.rs` — framing, CRC-32C, header roundtrip.
- `src/cap.rs` — CBOR capability-map parser.
- `src/transport.rs` — Sequencer + CompletionTable (7 cases).
- `src/{gpio,i2c,spi,pwm,iio}.rs` — wire builders + parsers.

Run from a Rust-for-Linux kernel tree with `CONFIG_KUNIT=y`:

```sh
./tools/testing/kunit/kunit.py run \
    --kconfig_add CONFIG_RPBRIDGE=m \
    --kconfig_add CONFIG_RPBRIDGE_INLINE_TESTS=y
```

These are the primary regression fence for wire behaviour. They
catch any change that would make the driver and firmware diverge
byte-for-byte.

> **Note.** A standalone `cargo test` harness for host-side
> verification is attractive but requires non-trivial shimming of
> the `kernel` crate's `Arc`/`Mutex`/`KVec`/`KString`/`pr_info!`/
> `#[vtable]` surface. Rust-for-Linux's in-kernel KUnit path is
> the current state of the art and is what this project uses.

## Layer 2 — `kunit/` (in-kernel)

C-based KUnit tests that are compiled alongside `rpbridge.ko` with
`CONFIG_KUNIT=y`. They exercise:

- CRC-32C parity with the firmware's `rpbp_crc32c.c`.
- RPBP header parse / validate against a golden vector.
- `RpbridgeDriver::probe()` minimal path (state allocation, table
  creation — no USB traffic expected on a unit-test harness).

Run (from a kernel build with Rust enabled):

```sh
./tools/testing/kunit/kunit.py run --kconfig_add CONFIG_RPBRIDGE=m \
    --kconfig_add CONFIG_RPBRIDGE_KUNIT=y
```

## Layer 3 — `umockdev/` (replay)

Scripted umockdev recording of an RPBridge enumerating on USB plus a
replay script that loads `rpbridge.ko` against the fake device and
asserts the expected aux_device children show up under
`/sys/bus/auxiliary/devices/`.

Files:

- `umockdev/rpbridge-attach.umockdev` — `umockdev-record` capture
  template (regenerate from a real device once hardware is ready).
- `umockdev/replay.sh` — driver-under-test runner. Fails with a non-
  zero exit status on any missing expected child.

## Layer 4 — `e2e.md`

Hand-driven hardware checklist. Every box on the list is a command
the operator runs against a plugged-in RPBridge. The document is the
sign-off gate before a firmware / driver release; CI cannot run it.

## Shared golden-vector corpus

Layers 1 and 2 consume the repo-wide golden vectors under
`tests/golden/` (see that directory's README). Adding a new wire
feature means:

1. Extend the firmware's `gen_golden` utility to emit the new vector.
2. Re-run `cmake --build . --target gen-golden` and check in the new
   `.hex` file.
3. Both `host/` Cargo tests and `kunit/` C tests auto-include any
   `.hex` file in the directory — no manifest maintenance.
