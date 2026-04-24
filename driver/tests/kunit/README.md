# KUnit tests for rpbridge

C-based KUnit tests that exercise the driver inside a kernel build.
Rust-for-Linux `kernel::kunit` is gated on target-kernel version
availability — when that interface is stable across every supported
kernel, these C tests will be migrated into `#[cfg(test)] mod`
blocks alongside the rest of the inline Rust tests.

## What each case covers

| Case | Purpose |
| --- | --- |
| `test_crc32c_rfc3720` | Reference vector parity. Firmware, userspace, and kernel Rust implementations MUST produce `0xE3069283` for ASCII `"123456789"`. |
| `test_crc32c_empty_input` | Zero-length input returns `0` — matches `rpbp_crc32c.c` and `userspace/rpbridge-protocol`. |
| `test_golden_sys_echo_crc` | The canonical SYS ECHO `CMD_RESPONSE` frame under `tests/golden/sys_echo.hex` has a checked CRC trailer. Catches endian/byte-order regressions. |

## Running

```sh
# From the kernel tree:
./tools/testing/kunit/kunit.py run \
    --kconfig_add CONFIG_USB=y \
    --kconfig_add CONFIG_AUXILIARY_BUS=y \
    --kconfig_add CONFIG_RPBRIDGE=m \
    --kconfig_add CONFIG_RPBRIDGE_KUNIT=y
```

## Symbol export

The Rust driver exposes `rpbridge_crc32c_slice` as a `no_mangle`
function gated on `CONFIG_RPBRIDGE_KUNIT=y`. The Kconfig entry lives
in `driver/Kconfig`; the export shim lives in a single `#[cfg(...)]`
block inside `src/core_driver.rs` that is compiled out on release
builds so production users see no exported symbol.

## Adding a new case

1. Decide whether the property is *kernel-context-dependent* (e.g.
   needs `struct device`, IRQ, or a sleeping lock). If not, add it
   as a `#[cfg(test)]` case inside the relevant Rust source and
   skip this directory entirely.
2. For kernel-context cases, add a `test_<name>()` function here,
   append to the `rpbridge_test_cases[]` array, and document its
   purpose in the table above.
