# USB VID / PID — allocation status & policy

## Current status

| Item | Value | Status |
| --- | --- | --- |
| **VID** | `0x1209` | Permanent — pid.codes Open Hardware vendor ID |
| **PID** | `0xFFFE` | **Pre-production** (testing-range, shared) |
| **bcdDevice** | `0x0100` | Bumped per protocol-major release (RPBP v1 → 0x0100) |

The PID `0xFFFE` is from the
[pid.codes testing range](https://pid.codes/howto/) `0xFFF0..0xFFFF`.
Any developer who has not yet completed a pid.codes registration
shares it. Two RPBridge boards on the same Linux host while using
this PID would collide with any other testing-range device on that
host. Acceptable for development, **unacceptable for production
hardware shipments**.

## Allocation strategy

We use **one VID/PID for the entire device**, not one per
subsystem. Subsystems are addressed via USB **interface descriptors**
inside the composite device (the standard USB approach used by every
serious multi-class peripheral, e.g. the Microsoft Surface Dock or
the FTDI FT4232H):

| Interface | bInterfaceClass / Subclass | Bound by |
| --- | --- | --- |
| 0–7 (varies by board) | CDC-ACM (`0x02`) | inbox `usbser` (Linux) / `Usbser.sys` (Windows) |
| n   | Vendor-class `0xFF` / `0x52` (`'R'`) | `rpbridge.ko` (Linux) / WinUSB via MS OS 2.0 (Windows) |
| n+1 | Vendor-class `0xFF` / `0x43` (`'C'`) | mainline `gs_usb.ko` (Linux) / `candle_api` (Windows) |

The Subclass byte is the disambiguator — `'R'` for the RPBP
transport, `'C'` for the gs_usb CAN interface. Linux's USB core uses
the `(idVendor, idProduct, bInterfaceClass, bInterfaceSubClass)`
tuple in the driver's match table, so each driver binds only its own
interface and does not clash with the others.

### Why not per-variant PIDs?

`rpbridge_rp2354b` and `rpbridge_pico2` ship the same VID/PID. The
disambiguator is the **`board:` string in the CBOR capability map**
(`"rpbridge-rev-B"` vs `"rpbridge-pico2"`), which the driver
exposes under `/sys/bus/usb/devices/.../board` so udev rules can
match on it.

Reasons:

- One pid.codes registration covers every variant.
- udev/policy can still distinguish — by board string, by serial,
  or by the capability-map subsystem set.
- No driver-side variant table to maintain — a new board variant
  is "new firmware build", not "new VID/PID + driver patch".

We retain the option to allocate per-variant PIDs later if a
deployment scenario demands strict udev-only routing.

## Pre-production gates (must complete before public release)

- [ ] **File pid.codes registration**. Submit a wiki entry at
      <https://pid.codes/howto/> with:
      - Repository URL
      - Project description (one paragraph)
      - Maintainer contact
      - Requested PID range (single PID is fine for v1)
- [ ] **Replace `RPBRIDGE_USB_PID = 0xFFFE`** with the assigned
      PID in:
      - `firmware/src/usb/usb_descriptors.c` (line ~50)
      - `driver/src/core_driver.rs` (line ~338)
      - This document (status table above)
- [ ] **Bump `bcdDevice`** to track allocation date (e.g.
      `0x0100` → `0x0101` once the real PID lands).
- [ ] **Regenerate udev rules** in `driver/udev/` if any rule
      hardcoded the testing PID.
- [ ] **Regenerate the umockdev recording** under
      `driver/tests/umockdev/` so CI replays against the
      production VID/PID.
- [ ] **Document in CHANGELOG** under "Changed" with the
      allocation date.

## Why pid.codes (and not USB-IF directly)

| Option | Cost | Time | Fit |
| --- | --- | --- | --- |
| pid.codes | free | 1–2 weeks (volunteer-run wiki) | ✓ Open hardware, public repo, individual-developer projects |
| OpenMoko `0x1d50` | free | similar | ✓ alternate Open Hardware VID; equivalent fit |
| USB-IF Vendor ID | $6000 once + $1500/yr | weeks | overkill until commercial volume justifies it |

pid.codes is the right choice for a development-product
open-source project. We can migrate to a USB-IF VID later without
breaking deployed devices — bcdDevice carries the variant info if
needed.

## References

- pid.codes registry — <https://pid.codes/>
- USB Implementers Forum VID policy — <https://www.usb.org/getting-vendor-id>
- ADR-0006 — locks UART = CDC + CAN = gs_usb, defining the
  interface-class layout above.
- ADR-0008 — locks the same-VID-PID-per-variant strategy.
