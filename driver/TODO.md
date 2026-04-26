# Driver — implementation roadmap

Track which piece of `rpbridge.ko` is implemented (`[x]`) or still
pending (`[ ]`). Each entry points to the C source file and the
authoritative spec / kernel binding.

## Implementation status

The driver was reset from the original Rust-for-Linux scaffold to
a clean C implementation on 2026-04-26 (see ADR-0003 amendment).
The previous Rust attempt is preserved on the `archive/rust-driver`
git branch.

### Core (USB driver entry, transport, capability map)

- [x] `src/main.c` — USB driver registration; `probe()` /
      `disconnect()` with kref-based lifetime; `module_init` /
      `module_exit` register the per-subsystem auxiliary_drivers
      in dependency order.
- [x] `src/rpbp.h` — wire constants (msg-types, subsystems, status
      codes, opcode namespaces, flag bits, VID/PID).
- [x] `src/rpbp_framing.{c,h}` — header encode/parse, full-frame
      build/validate, status-to-errno mapper. Defensive size + null
      checks at every entry; FRAGMENT+LAST contradiction caught.
- [x] `src/rpbp_crc32c.{c,h}` — Castagnoli CRC-32C with a baked-in
      lookup table. Independent of `<linux/crc32c.h>` so the driver
      loads on stripped distro kernels without `CONFIG_LIBCRC32C`.
- [x] `src/transport.{c,h}` — synchronous CMD_REQUEST/CMD_RESPONSE
      round-trip over the bulk pair. Per-device transport_lock
      serialises in-flight requests; seq_lock guards the rolling
      sequence allocator. All allocations kfree'd on every exit
      path, including timeouts and -ENOMEM.
- [x] `src/cap.{c,h}` — RFC 8949 §3 CBOR walker. Parses
      proto / fw / board / serial / mtu / buses with bounded
      allocations and per-container length validation. Unknown
      keys skipped (forward-compat).
- [x] `src/session.{c,h}` — HELLO + GET_CAPABILITIES glue used
      once at probe(). HELLO refuses to attach with
      -EPROTONOSUPPORT on a major-version mismatch.
- [x] `src/auxbus.{c,h}` — auxiliary_device spawn dispatcher with
      a class allow-list + per-device kref tracking + safe revoke
      path. Children hold an extra parent-kref so a still-active
      callback during disconnect() doesn't race the kref release.
- [x] `src/rpbridge.h` — shared per-device state struct.

### Subsystem children — 100 % of firmware-exposed buses covered

| Firmware subsys | Linux binding      | Source          | Userspace path                       |
| --------------- | ------------------ | --------------- | ------------------------------------ |
| GPIO    (0x03)  | `gpio_chip`        | `src/gpio.c`    | `/dev/gpiochipN`, libgpiod           |
| I²C     (0x01)  | `i2c_adapter`      | `src/i2c.c`     | `/dev/i2c-N`, i2c-tools              |
| SPI     (0x02)  | `spi_controller`   | `src/spi.c`     | `/dev/spidevN.M`                     |
| PWM     (0x04)  | `pwm_chip`         | `src/pwm.c`     | `/sys/class/pwm/pwmchipN/`           |
| ADC     (0x05)  | `iio_dev` (DIRECT) | `src/iio.c`     | `/sys/bus/iio/devices/iio:deviceN/`  |
| LED     (0x07)  | misc cdev          | `src/led.c`     | `/dev/rpbridge-led<N>`               |
| DMX     (0x08)  | misc cdev          | `src/dmx.c`     | `/dev/rpbridge-dmx<N>`               |
| OneWire (0x09)  | `w1_bus_master`    | `src/onewire.c` | `/sys/bus/w1/`                       |
| I²S     (0x0A)  | misc cdev          | `src/i2s.c`     | `/dev/rpbridge-i2s<N>`               |
| UART    (0x06)  | mainline `cdc_acm` | —               | `/dev/ttyACMN` (kernel-native)       |
| CAN             | mainline `gs_usb`  | —               | `can0` SocketCAN (kernel-native)     |

## Validation status

Honest state of the C driver:

- **Compile against Linux 7.0** — **✓ green in CI**.
  `tools/build.sh driver` is in the GitHub Actions matrix; 0
  warnings, 0 errors against the Ubuntu 26.04 LTS HWE kernel
  headers.
- **Compile against earlier 5.15+** — **☐ not verified.**
  The auxiliary_bus baseline is 5.15 LTS; we only test against
  7.0 in CI. Distros listed in `kernel-baseline/SUPPORT.md` are
  best-effort for now.
- **umockdev replay capture** — **☐ missing.**
  `tests/umockdev/rpbridge-attach.umockdev` + `.ioctl` need to
  be captured on real hardware once available. `replay.sh` fails
  with exit 1 ("missing fixture") until then.
- **Real hardware attach** — **☐ not tested.**
  The 10-section hardware checklist in `tests/e2e.md` was written
  against the Rust scaffold; the C driver inherits the same probe
  sequence so it should still apply, but no section has been
  executed against rev-B yet.
- **Static analysis (checkpatch)** — **☐ not in CI.**
  `make checkpatch` works locally; not yet a CI gate.

**Before calling the driver "validated" the open rows need to flip
to ✓.** Anyone with a 5.15+ kernel + RPBridge rev-B hardware can
close them.

## Open work — priority order

### P1 — Validation gates

- [ ] **umockdev recording capture** — run `umockdev-record` on
      real hardware, commit the resulting `.umockdev` + `.ioctl`
      files so `replay.sh` actually runs in CI.
- [ ] **E2E hardware run** — execute `tests/e2e.md` end-to-end;
      record sign-off block at bottom with firmware SHA + driver
      SHA + observations.
- [ ] **kunit-in-QEMU** — port the `tests/kunit/kunit_probe.c`
      stubs to plain C (today they're Rust-flavoured stubs left
      over from the previous scaffold) and wire them as a CI gate.
- [ ] **checkpatch in CI** — `tools/build.sh test` already calls
      `make checkpatch`; surface its exit status to the workflow
      so it gates the merge.

### P2 — Subsystem feature parity gaps

- [ ] **GPIO IrqChip** — nested `irq_chip` on top of the gpio_chip
      so `gpiomon` sees rising/falling edges with <5 ms latency.
      Blocked on the async event-channel kthread (P3).
- [ ] **GPIO mask atomic** — wire `set_multiple` / `get_multiple`
      gpio_chip callbacks to RPBP `SET_MASK` / `GET_MASK` so multi-pin
      operations get one round-trip instead of N.
- [ ] **I²C SMBus + NOSTART** — advertise `I2C_FUNC_NOSTART` and
      coalesce write-then-read across `master_xfer`'s msg[] to fold
      repeated-start patterns into a single RPBP XFER.
- [ ] **SPI manual CS** — expose RPBP `CS_ASSERT` / `CS_RELEASE`
      through a vendor ioctl on `/dev/spidev` so userspace
      multi-transaction flows can hold CS across multiple
      transfer_one calls.
- [ ] **IIO triggered buffer** — `iio_readdev -b ... -s ...`
      streaming via `iio_buffer` + `iio_trigger`. Needs the async
      event-channel kthread (P3) to consume STREAM_DATA frames
      outside the request/response path.

### P3 — Async event channel (unblocks P2 IRQ + streaming)

- [ ] **Event/stream kthread** — dedicated kernel thread that
      runs only when a subsystem has active subscribers; consumes
      EVENT + STREAM_DATA frames off the bulk-IN endpoint
      asynchronously. Reuses transport.c's frame validation but
      adds a per-channel completion table.

### P4 — Subsystem feature additions

- [ ] **LED multicolor leds-class** — register a per-strip
      `kernel/leds/leds-multicolor` device that maps the first
      pixel's R/G/B to the standard sysfs brightness interface.
      Keeps `/dev/rpbridge-ledN` for full-strip control; the
      leds-class hook is for status-LED-style userspace tooling.
- [ ] **DMX universe-mmap** — let userspace `mmap()` the 513-byte
      universe buffer; bring the OLA "rpbridge" plugin online
      against a stable mmapped ABI.
- [ ] **I²S ASoC machine + codec** — full ASoC stack (machine +
      codec + DAI link) so PCM-via-aplay works alongside the
      raw-write misc cdev path.

### P5 — Upstream-submission prep

- [ ] **In-tree builds** — Kbuild `rpbridge.ko` compiles in-tree
      under `drivers/usb/misc/rpbridge/`.
- [ ] **Checkpatch clean** — pass `scripts/checkpatch.pl --strict`
      on every source file (CI gate from P1).
- [ ] **`Documentation/` build** — `sphinx-build` clean (driver
      gets a `Documentation/usb/rpbridge.rst`).
- [ ] **RFC post** — `linux-usb@`. The auxiliary-bus child split
      keeps the per-subsystem maintainers happy reviewing only
      their slice.
