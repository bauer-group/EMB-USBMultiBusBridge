# USB VID / PID — allocation status & policy

## Current status

| Item | Value | Status |
| --- | --- | --- |
| **VID** | `0x0545` | **Production** — assigned to BAUER GROUP |
| **PID** | `0x0101` | **Production** — RPBridge |
| **bcdDevice** | `0x0100` | Bumped per protocol-major release (RPBP v1 → `0x0100`) |
| **Allocation date** | 2026-04-27 | First-shipping firmware: v1.0.0 (post-allocation) |

The pair is wired into the firmware descriptors, the kernel
driver's USB id_table, the userspace client's defaults, and the
shipped udev rules. Lock-step sites:

- `firmware/src/usb/usb_descriptors.c` — `RPBRIDGE_USB_VID` /
  `RPBRIDGE_USB_PID`
- `driver/src/rpbp.h` — `RPBRIDGE_USB_VID` / `RPBRIDGE_USB_PID`
- `driver/udev/70-rpbridge.rules` — `ATTRS{idVendor}` /
  `ATTRS{idProduct}`
- `userspace/rpbridge/src/lib.rs` — `DEFAULT_VID` / `DEFAULT_PID`

A mismatch on any one of those silently breaks the auto-bind path
— the kernel sees an unmatched device and never fires the driver's
probe(), userspace tooling can't find a controller. Always edit
all four together.

## Descriptor strings

| Field | Value |
| --- | --- |
| `iManufacturer` | `BAUER GROUP` |
| `iProduct` | `RPBridge` |
| `iSerialNumber` | `RPB-<16hex>` (e.g. `RPB-1A2B3C4D5E6F7081`) |

The serial number is derived deterministically from the W25Q
flash chip's 64-bit unique ID via
`pico_get_unique_board_id()`. Same physical board always produces
the same serial; no production-time provisioning step required.
Globally unique by manufacturer guarantee.

### Per-interface labels (`iInterface`)

| Interface | Class / Subclass | iInterface | Bound by |
| --- | --- | --- | --- |
| CDC-ACM #0 | `0x02 / 0x02` | `RPBridge UART1 (RS232)` | `cdc_acm` (Linux), `usbser.sys` (Windows) |
| CDC-ACM #1 | `0x02 / 0x02` | `RPBridge UART1 (RS485)` | `cdc_acm` / `usbser.sys` |
| CDC-ACM #2 | `0x02 / 0x02` | `RPBridge UART1 (TTL)` | `cdc_acm` / `usbser.sys` |
| CDC-ACM #3 | `0x02 / 0x02` | `RPBridge UART2 (TTL)` | `cdc_acm` / `usbser.sys` |
| CDC-ACM #4 † | `0x02 / 0x02` | `RPBridge Debug Log` | `cdc_acm` / `usbser.sys` |
| Vendor RPBP | `0xFF / 0x52` (`'R'`) | `RPBridge RPBP transport` | `rpbridge.ko` / WinUSB via MS OS 2.0 |
| Vendor CAN | `0xFF / 0x43` (`'C'`) | `RPBridge CAN (gs_usb)` | mainline `gs_usb.ko` / `candle_api` |

† — only present when the firmware is built with
`-DRPBRIDGE_DEBUG_CDC=ON`. With the flag ON, the vendor interfaces
shift their `bInterfaceNumber` from `{8, 9}` to `{10, 11}` to make
room for the new CDC at `{8, 9}`; the `MS OS 2.0` descriptor in
[`firmware/src/usb/ms_os_20.c`](../firmware/src/usb/ms_os_20.c)
mirrors the same gate so Windows WinUSB auto-binding follows. The
`iInterface` strings, the VID/PID, and the host-driver match keys
do not change. Default builds ship without the debug CDC — see
[`firmware/README.md`](../firmware/README.md) §Debugging for the
build-time toggle and the stdio routing rationale.

The subclass byte is the disambiguator — `'R'` for the RPBP
transport, `'C'` for the gs_usb CAN interface. Linux's USB core
uses the `(idVendor, idProduct, bInterfaceClass, bInterfaceSubClass)`
tuple in the driver's match table, so each driver binds only its
own interface and does not clash with the others.

## Allocation strategy

We use **one VID/PID for the entire composite device**, not one
per subsystem. Subsystems are addressed via USB **interface
descriptors** inside the composite device — the standard approach
used by every serious multi-class peripheral (e.g. the FTDI
FT4232H).

### Same VID/PID across board variants

`rpbridge_rp2354b` and `rpbridge_pico2` ship the same VID/PID.
The disambiguator is the `board:` string in the CBOR capability
map (`"rpbridge_rp2354b"` vs `"rpbridge_pico2"`), which the
driver exposes for userspace introspection so udev rules can
match on it if needed.

Reasons:

- One USB-IF allocation covers every variant.
- udev / policy can still distinguish — by board string, by
  serial, or by the capability-map subsystem set.
- No driver-side variant table to maintain — a new board variant
  is "new firmware build", not "new VID/PID + driver patch".

We retain the option to allocate per-variant PIDs from the same
VID later if a deployment scenario demands strict udev-only
routing.

## References

- USB Implementers Forum VID directory —
  <https://www.usb.org/sites/default/files/vendor_ids.pdf>
- ADR-0006 — locks UART = CDC + CAN = gs_usb, defining the
  interface-class layout above.
- ADR-0008 — locks the same-VID-PID-per-variant strategy.
