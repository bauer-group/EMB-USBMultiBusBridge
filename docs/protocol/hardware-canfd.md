# CAN-FD — hardware gate

**Status:** Reserved / not-on-rev-B. Software path is ready; hardware
is not. The protocol-layer cost to switch on once FD-capable silicon
is present is ~50 LoC in `src/usb/gs_usb_glue.c`.

## Why CAN-FD is not available on rev-B

The RPBridge's CAN transceiver path is implemented in firmware via
[can2040](https://github.com/KevinOConnor/can2040) — a pure-software
CAN controller that runs on the RP2350's PIO engine. can2040 is an
excellent Classic-CAN implementation (500 kbit/s, 1 Mbit/s stable on
rev-B silicon), but it is Classic-CAN **only**:

- The PIO state machine programs in `can2040.pio` are hardcoded for
  Classic-CAN 2.0B frame geometry (11-bit or 29-bit ID, DLC ≤ 8,
  single-rate bit timing).
- `struct can2040_msg` carries a fixed `uint8_t data[8]` payload.
- No bit-rate-switch (BRS) mode; no FD CRC polynomial; no ESI flag.

A software extension to can2040 for CAN-FD is not on the upstream
roadmap and would require rewriting the bit-engine for FD's dual
bit-rate, 64-byte payload, and different CRC.

## Paths to enable CAN-FD on a future board

1. **External SPI-attached FD controller.** Microchip MCP2518FD or
   NXP TJA1043 + TCAN4550 both expose a standard SPI register bank.
   Adds a small daughterboard footprint; the firmware grows a
   dedicated driver in `src/peripherals/` and `gs_usb_glue.c` gains
   an FD path that advertises `GS_CAN_FEATURE_FD` and maps
   `canfd_frame` payloads.
2. **RP2350 with an integrated CAN-FD core** (a hypothetical future
   silicon revision). Would replace can2040 entirely.
3. **Wait for a software CAN-FD PIO implementation.** Technically
   possible but not attempted by anyone as of this writing; would be
   a significant engineering project in its own right.

Options 1 and 2 are the realistic production paths.

## How the firmware is structured for the future

The gs_usb translator already declares all gs_usb wire constants that
a CAN-FD path needs:

- `GS_CAN_MODE_FD` — host opts in at `can_up` time.
- `GS_CAN_FEATURE_FD` — advertised in `gs_device_bt_const.feature`.
- The `gs_host_frame` C struct leaves room for the FD-payload
  extension via the wire-size selector (`host_frame_size()`).

Enabling CAN-FD on a future board therefore reduces to:

1. Replace / supplement `src/peripherals/can.c` with an FD-capable
   backend (exposing `struct can_hw_frame_fd` with `data[64]`, `len`,
   `brs`, `esi`).
2. Extend `struct gs_host_frame` with the FD payload layout.
3. Extend `host_frame_size()` to return 76 / 80 bytes when
   `GS_CAN_MODE_FD` is set.
4. Add an FD bit-timing control path (`GS_USB_BREQ_DATA_BITTIMING`,
   `GS_USB_BREQ_BT_CONST_EXT`).
5. Toggle `RPBRIDGE_HAVE_CANFD` at build time.

The RPBP protocol itself is not touched — CAN remains on the separate
gs_usb vendor interface (ADR-0006). This is the whole point of
keeping CAN off RPBP: the feature path is mainline-defined.

## Capability-map stance

Until FD hardware exists, the capability map does **not** advertise
`"can.fd"`. It advertises `"can.classic"` and `"can.hw-timestamp"`
only. Hosts SHOULD gate FD-only code on the presence of `"can.fd"`.

If a rev-C board ships, this document is updated with the actual wire
behaviour and the feature flag flipped on.

## References

- ADR-0006 — [UART via CDC-ACM only, CAN via gs_usb only](../adr/0006-uart-cdc-only-can-gs-usb-only.md).
- Mainline Linux `drivers/net/can/usb/gs_usb.c` — reference
  implementation of CAN-FD over the gs_usb protocol.
- can2040 upstream — https://github.com/KevinOConnor/can2040.
- Microchip MCP2518FD datasheet (DS20006027) — candidate external
  FD controller.
