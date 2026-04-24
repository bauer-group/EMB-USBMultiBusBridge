# ADR-0006 — UART via CDC-ACM only, CAN via gs_usb only

**Status:** Accepted — 2026-04-24
**Complements:** [ADR-0005 — Driver-optional coexistence](0005-driver-optional-coexistence.md)

## Context

[ADR-0005](0005-driver-optional-coexistence.md) established that the
Linux driver is *additive*: it provides kernel-native abstractions for
the subsystems CDC cannot express (GPIO / I²C / SPI / PWM / ADC), and
does not redefine UART or CAN on this device. The door was left open
for a future, per-instance driver takeover of UART via `UART_CLAIM`,
and — implicitly — for layering a second CAN protocol on top of RPBP
if the mainline `gs_usb` protocol turned out to be insufficient.

After weighing the engineering cost of pursuing either option against
the concrete feature gap, both doors are now closed.

## Decision

1. **UART is CDC-ACM only on both platforms, for the lifetime of v1.**
   - The `rpbridge.ko` driver will NOT register a `tty_driver`.
   - The firmware keeps `SYS UART_CLAIM` / `UART_RELEASE` opcodes and
     the `cdc_glue` arbiter branch as a *diagnostic / CI override*,
     but no first-party host code binds them. Third parties may.
   - `SET_PHY` / `GET_PHY` on the RPBP UART subsystem remain the sole
     phy-control API for scripts that cannot rely on DTR-driven
     auto-switch.

2. **CAN uses `gs_usb` only, for the lifetime of v1.**
   - The RPBP CAN subsystem will NOT be introduced. No CAN opcodes,
     no channel mapping, no driver code for CAN in `rpbridge.ko`.
   - Features that mainline `gs_usb` already has flags for —
     `GS_CAN_FEATURE_HW_TIMESTAMP`, `GS_CAN_FEATURE_FD`,
     `GS_CAN_FEATURE_BERR_REPORTING` — are implemented inside the
     existing `src/usb/gs_usb_glue.c` translator when the underlying
     hardware can deliver them.
   - `GS_CAN_FEATURE_FD` is gated on hardware: the current can2040
     PIO stack is Classic-CAN only (`struct can2040_msg.data[8]`
     is hard-coded). CAN-FD on this board is a future-hardware
     question (external SPI-attached MCP2518FD or equivalent), not
     a software question.

## Rationale

**Why not a custom UART protocol + driver?**

- CDC-ACM with the 4-per-phy layout + DTR-driven arbiter already
  covers ~95 % of terminal workflows (ADR-0005).
- A `tty_driver` in `rpbridge.ko` would duplicate line-coding,
  DTR/RTS, break, and flow-control state that `usbser` already
  handles on every Linux distribution we target. Net user benefit
  for the duplicated code is near zero; Windows parity is *lost*,
  not gained.
- `UART_CLAIM` is preserved as an opt-in escape hatch because the
  cost of keeping it is ~50 LoC of firmware and zero driver
  complexity — but no host code will bind it in v1.

**Why not a custom CAN protocol on top of RPBP?**

- SocketCAN over `gs_usb.ko` gives us `ip link set can0 up`,
  `candump`, `cansend`, `canplayer`, python-can, Kayak, Wireshark
  CAN dissector, Vector CANoe integration, and the full
  automotive-tool ecosystem for free.
- Windows integration via candle_api already works.
- Every feature we could imagine adding on a custom path
  (hardware timestamps, CAN-FD, error counters, filters) is either
  already a standardised `gs_usb` feature flag or fits cleanly as
  an additive flag. Extending `gs_usb_glue.c` keeps us in the
  mainline-compatible world.
- A second parallel path would break the driver-optional story:
  users would have to choose between SocketCAN and the custom
  driver; today they don't.

## Consequences

Positive:

- **Smaller surface area.** No RPBP CAN subsystem to specify,
  test, version, or document. No `tty_driver` to maintain.
  Firmware and driver stay lean.
- **Clearer driver scope.** `rpbridge.ko` is "GPIO/I²C/SPI/PWM/ADC
  over RPBP via auxiliary_bus" — a single sentence.
- **No ecosystem fragmentation.** SocketCAN users see `can0`;
  terminal users see `/dev/rpbridge-uart1-*`. Both paths are
  standard.
- **Upstream submission path.** `rpbridge.ko` can be submitted to
  linux-usb + linux-rust without CAN or tty code complicating the
  review.

Negative:

- **CAN-FD is gated on hardware.** If a user needs CAN-FD today, a
  board revision with an external FD-capable CAN controller is
  required. The protocol-layer cost to add FD later is small once
  the hardware exists (`gs_usb` has FD flags; the controller
  driver fills in `canfd_frame` instead of `can_frame`).
- **UART_CLAIM is dead-weight for v1.** Accepted — ~50 LoC, clearly
  documented as "reserved for future opt-in"; no runtime cost.

## Alternatives reconsidered

- **Bring up a custom `tty_driver` in `rpbridge.ko`.** Rejected —
  see "Why not a custom UART protocol + driver" above.
- **Replace `gs_usb` with RPBP-CAN.** Rejected — see "Why not a
  custom CAN protocol on top of RPBP" above.
- **Dual-path CAN (RPBP + gs_usb simultaneously).** Rejected —
  forces users to choose, complicates arbitration on shared
  hardware, and provides no feature that `gs_usb` cannot deliver
  once advertised via its feature flags.

## Implementation sequencing

1. Firmware **M10a** — `gs_usb_glue` advertises and emits
   `GS_CAN_FEATURE_HW_TIMESTAMP`. Every `gs_host_frame` carries
   a little-endian µs timestamp from `time_us_64()`. Cost: ~30
   LoC, mirrors the mainline `candle_api` wire format.
2. Firmware **M10b** — `gs_usb_glue` advertises
   `GS_CAN_FEATURE_FD` iff the compile-time flag
   `RPBRIDGE_HAVE_CANFD` is set (default: off). With can2040
   the flag stays off. Documented in
   `docs/protocol/hardware-canfd.md`.
3. Driver **M10c..i** — complete the RPBP client for the
   non-UART/non-CAN subsystems. See `driver/TODO.md`.
4. Tests **M10j** — umockdev recording, KUnit probe paths, and
   end-to-end smoke tests.

## References

- [ADR-0005](0005-driver-optional-coexistence.md) — the coexistence
  model this ADR further constrains.
- [ADR-0001](0001-protocol-choice.md) — RPBP framing stays as-is;
  no CAN opcodes are added.
- `firmware/src/usb/gs_usb_glue.c` — extended in M10a/M10b.
- `Documentation/networking/devlink/gs_usb.rst` (mainline kernel
  documentation) — source of the `GS_CAN_FEATURE_*` bit encoding.
- `drivers/net/can/usb/gs_usb.c` — the in-tree binding consumer.
