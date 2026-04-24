# Driver — implementation roadmap

Track which piece of `rpbridge.ko` is implemented (`[x]`) or still
pending (`[ ]`). Each entry points to the module, the authoritative
spec, and (where applicable) the upstream kernel binding it depends
on.

## M10 — RPBP client layer (kernel-binding agnostic)

All wire / Client / Sequencer / CompletionTable logic. Complete.

- [x] `src/rpbp.rs`, `src/cap.rs`, `src/transport.rs`.
- [x] `src/{gpio,i2c,spi,pwm,iio}.rs` wire + Client.

See `CHANGELOG.md` `[Unreleased] → Added` for the M10 bullet list.

## M11 — Driver fully functional via FFI (ADR-0007)

### M11a — FFI scaffolding

- [x] `src/ffi/mod.rs` + `src/ffi/bindings.rs` — hand-written
      `extern "C"` declarations pinned to Linux 6.13 LTS.
- [x] `src/ffi/shims.c` — C shim for inline helpers + struct-field
      accessors + typed object allocators.
- [x] Kbuild wires `src/ffi/shims.o` alongside `src/lib.o`.

### M11b — USB transport FFI

- [x] `src/ffi/wait.rs` — `WaitQueue` over `wait_queue_head_t`.
- [x] `src/ffi/usb.rs` — `Interface` + `BulkEndpoint` around
      `usb_bulk_msg` + endpoint walking.
- [x] `src/ffi/auxiliary.rs` — `parent_device()` helper (full
      auxiliary_bus wrapper is M12).

### M11c — Core driver

- [x] `core_driver::round_trip()` — synchronous request/response
      via the bulk IN/OUT pair. Validates CRC + header, correlates
      (channel, seq).
- [x] `core_driver::probe()` — end-to-end session: bulk-pair
      discovery → HELLO → GET_CAPABILITIES → spawn_children.
- [x] Stub disconnect() with a comment that aux children's Drop
      impls handle teardown.

### M11f–j — Subsystem kernel integration

- [x] **GPIO** — `gpio_chip` registered; `get` / `set` /
      `direction_input` / `direction_output` / `request` / `free`
      trampolines bridge to the Client. Accessible via
      `/dev/gpiochipN`, libgpiod, `gpioget` / `gpioset`.
- [x] **I²C** — `i2c_adapter` registered; `master_xfer` handles
      one message at a time with typed Write/Read directions.
      Accessible via `/dev/i2c-N`, `i2cdetect`, `i2cget` / `i2cset`,
      kernel drivers (sensors etc.).
- [x] **SPI** — `spi_controller` registered; `transfer_one_message`
      walks the transfer list and honours `cs_change`. Accessible
      via `spidev` on `/dev/spidevX.Y`, `spi-pipe`, `spidev_test`.
- [x] **PWM** — `pwm_chip` registered with `apply` + `get_state`.
      Accessible via `/sys/class/pwm/pwmchipN/`, the standard sysfs
      interface (export / period / duty_cycle / enable).
- [x] **IIO (single-shot)** — `iio_device` registered with
      `read_raw`. Voltage channels 0..3 and on-die temp channel 4
      exposed. Accessible via `/sys/bus/iio/devices/iio:deviceN/`,
      `iio_info`, `cat in_voltage0_raw`.
- [ ] **IIO (triggered buffer)** — `iio_readdev -b ... -s ...`
      streaming needs a kthread that consumes STREAM_DATA frames
      from the USB IN endpoint outside the request/response path.
      Tracked as M12; single-shot IIO is fully functional.

### M11k–l — Test coverage

- [x] `transport::MockRoundTrip` — deterministic RoundTrip impl
      with FIFO response queue + captured-request introspection.
- [x] 85+ KUnit-for-Rust test cases (via `#[cfg(test)] mod tests`
      + `mod client_tests` in every src/*.rs file):
      - Wire layer: 35+ cases (CRC, framing, every opcode encode
        + parse, fragmentation flags).
      - Sequencer + CompletionTable: 10 cases.
      - Client integration (MockRoundTrip): 50+ cases across all
        5 subsystems covering happy path, edge cases, and errno
        propagation.
- [x] `tests/kunit/` — C-based KUnit tests for CRC-32C parity
      against the firmware via `rpbridge_crc32c_slice` shim.
- [x] `tests/umockdev/` — replay script + expected-children
      manifest.
- [x] `tests/e2e.md` — 10-section hardware checklist.

## M12 — Post-delivery refactors (pre-upstream-RFC)

- [ ] **auxiliary_bus migration** — wrap each Child in
      `auxiliary_device_{init,register,delete,uninit}` per ADR-0002.
      Mechanical refactor; no behaviour change visible to userland.
- [ ] **Async event/stream kthread** — consumes STREAM_DATA + EVENT
      frames off the USB IN endpoint on a dedicated kernel thread.
      Unblocks GPIO WATCH edge events and IIO triggered-buffer
      streaming.
- [ ] **GPIO IrqChip** — nested irq_chip on top of the gpio_chip
      so `gpiomon` sees rising/falling edges with <5 ms latency.
- [ ] **I²C SMBus + NOSTART** — advertise `I2C_FUNC_NOSTART` and
      coalesce write-then-read across `master_xfer`'s msg[] to fold
      repeated-start patterns into a single RPBP XFER.
- [ ] **Safe-wrapper migration** — as each `kernel::<module>` binding
      lands upstream, delete the corresponding `src/ffi/<module>.rs`
      and rewire call sites. See ADR-0007 §Upstream-replacement
      plan for the expected order.

## Upstream-submission prep (future M13)

- [ ] `kernel-baseline/` bump to the R4L tree that provides all
      bindings we currently shim.
- [ ] Move `src/ffi/shims.c` content into safe Rust callers once
      the bindings allow it; `shims.c` should shrink to zero.
- [ ] Kbuild: ensure `rpbridge.ko` compiles in-tree under
      `drivers/usb/misc/rpbridge/`.
- [ ] Checkpatch + `sphinx-build Documentation/` clean.
- [ ] RFC post to `linux-usb@` / `rust-for-linux@`.
