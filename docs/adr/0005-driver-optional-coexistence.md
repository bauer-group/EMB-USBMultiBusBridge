# ADR-0005 — Driver-optional coexistence (always-on CDCs + vendor-only extras)

**Status:** Accepted — 2026-04-24
**Supersedes:** [ADR-0004 — Driver-priority mode via dual USB configuration](0004-driver-priority-mode.md)

## Context

[ADR-0004](0004-driver-priority-mode.md) proposed the USB
multi-configuration trick: keep the default `Compat` configuration
(4× CDC-ACM + 2× vendor) for driverless users, and let the Linux
driver re-enumerate the device to a `Native` configuration (vendor-
only) at `probe()` time. That design assumed the CDCs and the
driver-native UART could not share the UART0 hardware.

In the interim two things changed:

1. **M4e landed "per-phy CDC-ACMs with DTR auto-switch"** — the
   firmware now correctly arbitrates UART0 between three CDC-ACM
   instances (RS232 / RS485 / TTL) based on which CDC has DTR
   asserted. The same mechanism extends cleanly to RS485 once the
   DE-line timing was validated end-to-end (see
   [docs/protocol/subsystems/uart.md](../protocol/subsystems/uart.md)).
2. **The assumption "CDCs and driver-UART cannot coexist" turned
   out to be wrong.** If exactly one consumer holds UART0 at any
   given moment, and the choice is visible + controllable, the
   pipeline is unambiguous regardless of whether that consumer is
   a CDC or a kernel-driver byte stream.

We revisit the decision now because the re-enumeration dance of
ADR-0004 has non-trivial downsides (drops user-space file handles,
triggers Windows device-install sound, complicates USB-power
budget analysis) that are avoidable under the new mechanism.

## Decision

Adopt **single-configuration coexistence**:

- The device advertises **one** USB configuration. All 4 CDC-ACMs
  and both vendor interfaces are always enumerated. There is no
  `SET_CONFIGURATION` hop at driver load.
- **Driverless path** (current behaviour, unchanged): the host's
  inbox `usbser` driver binds each CDC; users open
  `/dev/rpbridge-uart1-rs232` etc. and type. DTR-driven phy
  auto-switch keeps UART0 HW allocated to whichever CDC the
  operator currently cares about.
- **Driver path** (new):
  - `rpbridge.ko` binds to the RPBP vendor interface and
    `gs_usb.ko` binds to the CAN vendor interface — both are
    vendor-class and do not collide with the CDCs.
  - The driver spawns `auxiliary_bus` children for every subsystem
    advertised in the capability map: `gpiochip`, `i2c-adapter`,
    `spi-master`, `pwmchip`, `iio-device`. The CAN interface keeps
    going straight through `gs_usb`.
  - The driver **does NOT** take over UART as a first-class
    `tty_driver`. The CDCs remain the canonical UART path. This
    avoids duplicating a working interface.
  - The driver MAY offer opt-in UART coordination for cases where
    a user needs tighter timing control than CDC can give — e.g.
    an `rpbridge.ko` sysfs attribute that requests a hand-off of a
    specific UART instance via a new RPBP `UART_CLAIM` opcode.
    Firmware signals the relevant CDC to silently drain writes for
    the duration of the claim. This is an *optional enhancement*,
    not the default.

The Linux driver is therefore strictly *additive*: it provides
Linux-native abstractions for the subsystems CDC cannot express
(GPIO / I²C / SPI / PWM / ADC). It does not redefine how UART or
CAN work on this device.

## Consequences

Positive:

- **No re-enumeration.** Plugging in the device produces a single
  stable device topology. Load and unload of `rpbridge.ko` does
  not churn file handles or trigger Windows "device install"
  notifications.
- **Both audiences are first-class simultaneously.** A user running
  a Python script against `/dev/i2c-3` (via the driver) can open
  a terminal on `/dev/rpbridge-uart1-rs232` at the same time — no
  mode switch, no conflict.
- **Simpler firmware.** No `bNumConfigurations = 2`, no
  `tud_descriptor_configuration_cb(index)` selector, no
  `iConfiguration` strings. The USB descriptor set shipped today is
  already the final shape.
- **Simpler driver.** `rpbridge.ko` does `probe()` → spawn aux
  children. No `usb_driver_set_configuration()` round-trip, no
  re-probe logic. Kernel-module reload semantics match every other
  Linux USB driver.
- **Windows parity.** The same single configuration + MS OS 2.0
  binding that works on Linux works on Windows unchanged. `gs_usb`
  via `candle_api`, RPBP via WinUSB, CDCs via inbox `usbser`.

Negative:

- **UART control ergonomics are deferred.** A user who wants the
  driver to fully own a UART (e.g. for hard real-time termios
  control) will need to opt in per-instance via the UART_CLAIM
  coordination path. That API does not exist yet; the default
  driver experience will route UART through the existing CDCs.
  Acceptable — CDCs cover 95 % of terminal workflows already.
- **Feature discoverability.** With one configuration, the
  capability map has to flag which subsystems are driver-only
  (GPIO / I²C / SPI / PWM / ADC) vs CDC-available (UART / CAN)
  vs both. A new capability-map field `"access"` (values:
  `"cdc"`, `"vendor"`, `"cdc+vendor"`) makes this explicit. Small
  amount of doc work.

## Alternatives re-considered

- **ADR-0004 dual configuration** — still functionally correct,
  but the re-enumeration cost and the complexity in the driver
  `probe()` path outweigh the marginal benefit of fully hiding
  the CDCs. Superseded.
- **Two USB PIDs** — previously rejected for the same reason:
  forces users to re-flash to switch modes. Still wrong answer.
- **Driver owns UART + CDCs disappear via detach()** — would
  require the driver to unbind `usbser` per-instance, which is
  invasive and not portable to distributions that ship different
  `usbser` permission models.

## Implementation sequencing

1. Firmware **M9a** — docs only. Update capability map with
   `"access"` tagging per subsystem. Update
   `docs/protocol/capabilities.md` schema and
   `firmware/src/capabilities/capabilities.c` emitter. Trivial.
2. Firmware **M9b** — optional UART_CLAIM / UART_RELEASE opcodes
   (SYS subsystem, new opcodes). Firmware holds a per-UART-instance
   "claimed by driver" flag; `cdc_glue.c` silently drains writes
   from CDCs while the flag is set. Writes coming back through the
   vendor pipe via RPBP are preferred. Non-blocking for M9 delivery;
   implement when the driver grows need for it.
3. Driver **M9c** — `rpbridge.ko` probes the RPBP vendor interface,
   walks the capability map, spawns matching `auxiliary_bus`
   children. No `SET_CONFIGURATION` required (the vendor interface
   is already enumerated). See `driver/src/core_driver.rs`.
4. Driver **M9d** — subsystem children (gpio / i2c / spi / pwm /
   iio). Each translates its kernel-ABI calls into RPBP commands on
   the vendor pipe.
5. Documentation — update this ADR with real-world findings from
   the first driver test cycle; retire `0004-driver-priority-mode.md`
   from the ADR index once the code is stable.

## Supersedes / complements

- **Supersedes** [ADR-0004 — Driver-priority mode via dual USB
  configuration](0004-driver-priority-mode.md). The
  re-enumeration mechanism described there is no longer needed
  and is not scheduled for implementation.
- **Complements** [ADR-0001 — USB protocol framing choice](0001-protocol-choice.md):
  RPBP remains the transport on the vendor interface.
- **Complements** [ADR-0002 — Linux driver bus pattern](0002-driver-bus-choice.md):
  `auxiliary_bus` children remain the structural pattern for
  subsystem integration.

## References

- [ADR-0004](0004-driver-priority-mode.md) — the superseded design.
- USB 2.0 specification §9.2.3 — multi-configuration semantics
  (no longer exercised by this product).
- Linux `drivers/base/auxiliary.c` — the bus we spawn children on.
- `drivers/net/can/usb/gs_usb.c` — the in-tree driver that binds
  our CAN vendor interface unchanged.
