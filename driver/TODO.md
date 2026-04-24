# Driver — implementation roadmap

Track which piece of `rpbridge.ko` is implemented (`[x]`) or still
pending (`[ ]`). Each entry points to the module, the authoritative
spec, and (where applicable) the upstream kernel binding it depends
on.

## M10 — Driver-native RPBP client (per ADR-0006)

### M10c — Transport core

- [x] `src/transport.rs` — `Sequencer` + `CompletionTable` with
      pending / completed / dead states, cancel + reap semantics,
      7 unit tests.
- [x] `src/core_driver.rs` — `RpbridgeState` with `Inner` mutex,
      `RoundTrip` impl sketch, `dispatch_rx_frame` router for
      CmdResponse / Error / Event / StreamData, ERROR frame parser.
- [ ] **§R4L-USB-BINDINGS:** wire `submit_frame_blocking` against
      `kernel::usb::bulk_msg` once that binding lands in the
      target Rust-for-Linux tree (tracking 6.14 / 6.15).
- [ ] **§R4L-USB-BINDINGS:** wire `wait_for_completion` against
      `kernel::sync::CondVar::wait_timeout` once available.
- [ ] URB anchor + RX pool (N=4 bulk-IN URBs, anchored for
      `usb_kill_anchored_urbs` on disconnect).

### M10d — Capability-driven spawn

- [x] `core_driver::spawn_children` walks `caps.buses`, skips
      `"uart"` and `"can"` explicitly (ADR-0006), spawns the rest.
- [ ] **§R4L-AUX-BINDINGS:** wrap each returned `Child` in
      `auxiliary_device` + call `auxiliary_device_register`. Needs
      `kernel::auxiliary` bindings (WIP upstream).

### M10e — GPIO (gpio_chip + irq_chip)

- [x] `src/gpio.rs` wire layer (LIST / CONFIGURE / SET / GET /
      SET_MASK / GET_MASK / WATCH / UNWATCH + EdgeEvent parser).
- [x] `Client<T: RoundTrip>` — every op is one round-trip.
- [x] `GpioChild::probe()` — discovers pin whitelist via LIST.
- [ ] **§R4L-GPIO-BINDINGS:** register a `kernel::gpio::Chip`
      vtable. Map `get`/`set`/`direction_in/out`/`set_config` to
      Client calls.
- [ ] **§R4L-GPIO-BINDINGS:** nested `irq_chip` atop the Chip.
      Events channel pump in `core_driver::handle_event` dispatches
      parsed `EdgeEvent` payloads to `handle_nested_irq(virq)`.

### M10f — I²C adapter

- [x] `src/i2c.rs` wire layer (PROBE / XFER / SCAN / SET_FREQ /
      GET_FREQ) with NO_STOP flag.
- [x] `Client<T>` with `xfer_msg_list` that detects write-restart-
      read pattern and sets `FLAG_NO_STOP` on the write half.
- [ ] **§R4L-I2C-BINDINGS:** register `kernel::i2c::Adapter`.
      `master_xfer` maps `i2c_msg[]` to `xfer_msg_list`; functionality
      = `I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL`.

### M10g — SPI controller

- [x] `src/spi.rs` wire layer (XFER / SET_MODE / SET_FREQ /
      GET_FREQ / CS_ASSERT / CS_RELEASE).
- [x] `Client<T>` with `xfer_message` that handles `cs_change` for
      HOLD_CS, and `Mode` enum → CPOL/CPHA bit mapping.
- [ ] **§R4L-SPI-BINDINGS:** register `kernel::spi::Controller`.
      `transfer_one_message` calls `xfer_message(cs_pin, &xfers)`.

### M10h — PWM chip

- [x] `src/pwm.rs` wire layer (CONFIGURE / SET_DUTY / START / STOP
      / GET_STATE) + `encode_state` / `decode_state` converters
      between `pwm_state {period_ns, duty_ns}` and
      `{freq_hz, duty_q16}`.
- [x] `Client<T>` with `apply_state` (CONFIGURE + START/STOP).
- [ ] **§R4L-PWM-BINDINGS:** register `kernel::pwm::Chip`. `apply`
      calls `client.apply_state(pin, ...)`; `get_state` calls
      `decode_state` on a GET_STATE round-trip.

### M10i — IIO device + triggered buffer

- [x] `src/iio.rs` wire layer (READ / READ_MANY / GET_REF /
      TEMP_READ / STREAM_START / STREAM_STOP + STREAM_DATA parser).
- [x] `Client<T>` covering all 6 opcodes.
- [x] `ChanDesc` / `ChanKind` types mirror `iio_chan_spec` shape.
- [ ] **§R4L-IIO-BINDINGS:** register `kernel::iio::Device`.
      `read_raw` maps `IIO_CHAN_INFO_RAW / SCALE / PROCESSED` to
      Client calls. Triggered-buffer path: `buffer_postenable` →
      `stream_start`, event pump pushes samples via
      `iio_push_to_buffers_with_timestamp`, `buffer_predisable` →
      `stream_stop`.

### M10j — Tests

- [x] Inline `#[cfg(test)] mod tests` in every src/*.rs —
      exhaustive wire-format regression fence (35+ cases total).
- [x] `tests/kunit/` — C-based KUnit tests for CRC-32C parity with
      the firmware + golden-vector CRC trailer.
- [x] `tests/umockdev/` — replay script + expected-children
      manifest + recording regeneration procedure.
- [x] `tests/e2e.md` — 10-section hand-driven hardware checklist
      covering every claim in ADR-0005 and ADR-0006.

### M10 — Explicit non-goals (ADR-0006)

- [ ] **NOT** a `tty_driver`. CDC-ACM owns UART; UART_CLAIM stays
      as diagnostic escape hatch but has no driver-side consumer.
- [ ] **NOT** an RPBP-CAN subsystem. `gs_usb.ko` + gs_usb_glue.c
      is the sole CAN path on both Linux and Windows. CAN-FD is
      a hardware gate (see `docs/protocol/hardware-canfd.md`).

## Upstream-submission prep (future M11)

- [ ] `kernel-baseline/` bump to whatever R4L tree binds
      `kernel::usb::*` comprehensively.
- [ ] Kbuild: ensure `rpbridge.ko` compiles in-tree under
      `drivers/usb/misc/rpbridge/`.
- [ ] Checkpatch + `sphinx-build Documentation/` clean.
- [ ] First RFC post to `linux-usb@` / `rust-for-linux@` with
      `rpbridge: driver for RPBridge USB multi-bus bridge` as
      subject. Cover-letter links to this TODO and ADR-0006.
