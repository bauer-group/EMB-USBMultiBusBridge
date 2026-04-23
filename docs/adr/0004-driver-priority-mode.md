# ADR-0004 — Driver-priority mode via dual USB configuration

**Status:** Accepted — 2026-04-23

## Context

The firmware has two target audiences whose needs point in opposite
directions:

1. **Driverless user.** Plugs the device in, opens a terminal, types.
   Uses `minicom`, PuTTY, Arduino IDE, Tera Term. Expects zero setup.
   Today served by the four CDC-ACM virtual COM ports (one per
   phy-bank transceiver + one PIO-UART).
2. **Driver user.** Wants the device to look and feel like native
   Linux hardware: `/dev/gpiochipN`, `/dev/i2c-N`, `/dev/spidevN.M`,
   a first-class `tty_driver` backing `/dev/ttyRPB*` with full
   termios support, `can0` via SocketCAN, IIO ADC nodes, etc. This
   is what the Rust kernel driver (`driver/`) is being built for.

These two profiles have **incompatible USB interface requirements**:

- The driverless profile wants CDC-ACMs for every user-facing UART
  so inbox `usbser` can bind them without help.
- The driver-native profile wants to own every UART byte stream
  itself — CDC-ACMs are pure overhead at that point (CDC framing
  latency, FIFO copies, tty-core double-indirection).

Running them both simultaneously would mean having the CDC-ACMs and
the driver-native tty devices compete for the same UART hardware.
Even if we arbitrated the clash in firmware, the user experience
would be confusing (two `/dev/ttyXXX` names per phy, only one of
which actually carries traffic).

## Decision

Adopt the **USB Multi-Configuration** mechanism from the USB 2.0
spec §9.2.3. The device advertises two configurations:

- **`bConfigurationValue = 1` — "Compat"** (default, what ships
  today). 4× CDC-ACM + 2× vendor. Driverless-ready.
- **`bConfigurationValue = 2` — "Native"** (activated only when the
  `rpbridge` kernel driver is loaded). 2× vendor only; every UART /
  GPIO / I²C / SPI / PWM / ADC stream rides RPBP channels. The
  driver terminates those streams in the matching Linux subsystem
  core.

The Linux kernel driver handles the switch:

1. Linux host enumerates the device → picks Config 1 automatically
   (it's the first one listed). `usbser` binds the CDCs.
2. `rpbridge.ko` probes the RPBP vendor interface in Config 1.
3. During `probe()` the driver issues a USB
   `SET_CONFIGURATION(2)` on the device.
4. The device re-enumerates on Config 2 (2× vendor only); the CDCs
   disappear from the host, `usbser` releases their tty slots.
5. `rpbridge.ko` re-probes on Config 2 and spawns its
   auxiliary_bus children (gpio, i2c, spi, pwm, iio, tty, can
   shim). `/dev/ttyRPB-*` goes live with the full tty ABI.

If the user unloads the driver later, the device re-enumerates
back to Config 1 on the next USB reset, restoring driverless mode.

This matches the `usb.c` / `usb_reset_configuration()` pattern in
mainline drivers (see `drivers/usb/class/cdc-acm.c` for a nearby
example of explicit config selection).

## Consequences

Positive:

- **No behaviour regression for driverless users.** Config 1 is the
  default; zero new host-side action is required.
- **Driver users get real Linux semantics.** No more "pipe your
  bytes through rpbridge-ctl"; every subsystem speaks its canonical
  kernel ABI.
- **Mutually exclusive by construction.** Exactly one configuration
  is active at any time; no tty-double-bind or byte-duplication
  concerns.
- **USB-spec-conformant mechanism.** Not a hack.
- **Graceful fallback.** If a Linux kernel lacks `rpbridge.ko` (or
  the module fails to load), the device stays in Config 1 forever
  and functions perfectly.

Negative:

- **One-time ~500 ms reconnect** after the driver loads. The host
  sees a USB disconnect/reconnect sequence; user-space tools
  connected to the *old* CDC nodes will fail their next I/O and
  need to reopen against the new `/dev/ttyRPB*`. In practice the
  driver load happens very early in boot before any user opens
  a port, so this is invisible.
- **Descriptor set roughly doubles in RAM.** Two configuration
  descriptors + strings for each `iConfiguration`. ~80 extra bytes
  of Flash. Acceptable.
- **Firmware bookkeeping.** `cdc_glue.c` has to degrade gracefully
  when CDCs are not enumerated (they simply never come up). The
  existing `tud_cdc_n_ready()` guard already covers that, so no
  extra branches are needed.
- **Windows support lag.** Windows is not in the driver path
  currently — it sits in Config 1 indefinitely. This is consistent
  with the Linux-first project strategy.
- **Milestone ordering.** Config 2 is only useful once the native
  subsystems exist. We sequence it as M9, after M2 (I²C) / M3
  (SPI / GPIO / PWM / ADC) / M5 (CAN) have landed RPBP stream
  channels that the native tty/gpio/i2c drivers can consume.

## Alternatives considered

- **Single config, all vendor.** Drops driverless support entirely.
  Rejected — the core product value is "works anywhere".
- **Single config, dual-bind** (CDCs + vendor both present). Rejected
  — creates the two-tty-for-one-phy ambiguity described above.
- **Two product USB PIDs**, one for each mode. Rejected — requires
  users to re-flash the firmware to switch modes; also wastes
  pid.codes allocations.
- **Vendor-specific "extended CDC"** where RPBP overlays CDC
  semantics. Rejected — a custom not-quite-CDC descriptor set
  breaks inbox driver binding on Windows and macOS.

## Implementation sequencing

1. Firmware **M9a** — descriptor infrastructure: add Config 2 descriptor,
   `iConfiguration` strings, route `tud_descriptor_configuration_cb`
   by index.
2. Firmware **M9b** — per-config behaviour: verify the
   `cdc_glue_poll` / RPBP pumps survive a config switch, add an
   `EVENT`-frame on config change for host-side audit.
3. Driver **M9c** — `rpbridge.ko` `probe()` issues
   `SET_CONFIGURATION(2)`, re-binds on Config 2 match, spawns the
   auxiliary_bus children (gated on which subsystems exist).
4. Driver **M9d** — tty-core driver for the UARTs; maps RPBP stream
   channels to termios line-coding cleanly.
5. Documentation + ADR cross-links once M9c/d are on disk.

## Supersedes

Complements (does not replace):

- [ADR-0001 — USB protocol framing choice](0001-protocol-choice.md):
  RPBP remains the transport in both configurations.
- [ADR-0002 — Linux driver bus pattern](0002-driver-bus-choice.md):
  `auxiliary_bus` children are still the mechanism; only the
  *source* of their input streams differs between the two configs.

## References

- USB 2.0 specification §9.2.3 "Generic USB Device Operations"
  describes multi-configuration semantics and the
  `SET_CONFIGURATION` request.
- TinyUSB's `tud_descriptor_configuration_cb(uint8_t index)` is
  the extension point — one function returning different descriptor
  arrays per `index`.
- Linux kernel `usb_driver_claim_interface` / `usb_reset_configuration`
  — the driver-side API we will use for the switch.
