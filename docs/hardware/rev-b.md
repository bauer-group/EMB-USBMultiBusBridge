# Hardware — rev-B specification

The current shipping hardware revision is **rev-B**: a 2-layer,
80 × 55 mm PCB built around the Raspberry Pi **RP2354B** (RP2350B
die + 2 MB stacked QSPI flash in a QFN-80 package). Power is
USB-C bus-powered only — no external supply, no barrel jack, no
PD negotiation.

This page is the high-level board spec. The authoritative pin
assignments live in
[`firmware/boards/rpbridge_rp2354b.h`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/firmware/boards/rpbridge_rp2354b.h);
the per-GPIO breakdown is in
[pinout.md](pinout.md) (boot-time defaults) and
[pin-matrix.md](pin-matrix.md) (silicon-capability + reconfigurable
view). Use this page when you want the bird's-eye picture: what
the board is, what it exposes, and what the firmware expects from it.

## Highlights

- **Single discrete RGB LED** on GP22/GP23/GP24, common-anode,
  one current-limit resistor per channel. Driven via PWM with
  output-invert so duty=0 → LED off.
- **Single user button** on GP25 (front-panel push), pull-up via
  internal 50 kΩ.
- **One canonical connector per bus**, no jumper-soldering required:

  | Bus / signal | Connector                                   |
  |--------------|---------------------------------------------|
  | RS232        | DE-9 (DTE pinout)                           |
  | RS485        | Phoenix MSTBA 3.81 mm screw terminal        |
  | UART2 TTL    | 4-pin 2.54 mm header                        |
  | CAN 2.0B     | DE-9 (CiA 303-1 pinout)                     |
  | I²C bus 0    | 3× Qwiic JST-SH (`J3`) + 4-pin header       |
  | I²C bus 1    | Qwiic JST-SH (`J5`) + 4-pin header          |
  | SPI bus 0    | PMOD 2A (`J8`)                              |
  | SPI bus 1    | 9-pin breakout header (`J9`)                |
  | GPIO/PWM/ADC | 2×10 user header (`J12`)                    |
  | 1-Wire       | 3-pin 2.54 mm header                        |
  | DMX-512 (TX) | 5-pin XLR (user wires the RS-485 transceiver) |
  | LED strip    | 3-pin header (WS2812 / SK6812)              |
  | I²S TX       | 5-pin header (BCLK / LRCLK / DOUT / +3V3 / GND) |
  | Debug        | 5-pin SWD + UART1 stdio                     |

- **Real mechanical jumpers** (not solder bridges) for the CAN and
  RS485 120 Ω terminators — moved with fingernails, no iron needed.
- **PTC-fused USB input** with Schottky reverse-feed block and TVS
  clamp on the data lines.

BOM totals about **21 € per unit in singles, 11 € in 100s**.

## What the firmware expects from the hardware

Every pin assignment, transceiver mapping and feature flag is
already encoded in the rev-B board header. A custom variant of
this board needs to:

1. **Keep UART0 on GP0/GP1** if it wants the firmware's three-bank
   transceiver auto-switching (RS232 / RS485 / TTL). The phy module
   in `firmware/src/peripherals/uart_phy.c` hard-codes the
   shutdown / DE / EN pins via `RPBRIDGE_RS232_SHDN_PIN`,
   `RPBRIDGE_RS485_DE_PIN`, `RPBRIDGE_RS485_EN_PIN` — overridable
   in a custom board header.
2. **Keep CAN on GP20/GP21** unless you're prepared to retune the
   `can2040` PIO program. PIO0 is reserved for can2040.
3. **Honour the pin-broker whitelist** for runtime-claimable user
   pins. The rev-B header lists 11 pins on the 2×10 user header
   (`RPBRIDGE_USER_GPIO_PIN_LIST`) and 6 PWM-capable pins
   (`RPBRIDGE_USER_PWM_PIN_LIST`). The PIO applets (LED-strip,
   DMX, I²S, 1-Wire) live on whitelist pins too — the broker
   arbitrates.
4. **Provide a 4.7 kΩ external pull-up to +3V3** on the 1-Wire
   header (GP32). The PIO program emulates open-drain by toggling
   `pindirs`; the bus only goes HIGH because the pull-up does it.

## Composite USB layout

VID/PID = `0545:0101` (production, BAUER GROUP). The composite
device exposes:

- **4 × CDC-ACM** (5 with `-DRPBRIDGE_DEBUG_CDC=ON`) — three banks
  on UART0 hardware (RS232, RS485, TTL) plus UART2 over a
  PIO-UART. The firmware auto-selects the active phy via DTR.
- **2 × Vendor interface** — `RPBP transport` (subclass `'R'` /
  WinUSB via MS OS 2.0 / `rpbridge.ko` on Linux) and
  `gs_usb CAN` (subclass `'C'` / `gs_usb.ko` mainline / `candle_api`
  on Windows).

Full descriptor view at [`docs/usb-vid-pid.md`](../usb-vid-pid.md).
The CDC count drops automatically on bare-board variants — see the
`RPBRIDGE_BASE_CDC_COUNT` formula in the active board header.

## Power topology

- **Input**: USB-C VBUS (5 V nominal, 4.5–5.5 V tolerated). Hot-plug
  inrush is clamped by the PTC + bulk capacitance.
- **Reverse feed protection**: 1 A Schottky in series, blocks current
  flowing back into the host.
- **TVS clamp** on D+/D- and on VBUS for ESD protection.
- **Regulator**: 3.3 V LDO (rated 600 mA continuous), drives the RP2350
  core + every IO bank + transceiver ICs. Quiescent current under
  18 mA at idle (LED dimmed, no active CDC).
- **VBUS sense**: 100 kΩ / 49.9 kΩ divider into GP46 — used by the
  firmware's `SYS GET_VBUS` capability to report measured VBUS in
  millivolts. Useful for diagnostic / "is the host providing 5 V?"
  checks.

The board does **not** offer a "host-power" mode. There is no path
to drive externally-powered targets from the device's 3.3 V — every
header that brings out 3V3 is a *consume-from* rail, current-limited
to a few mA via the trace + PTC budget.

## Programming and debug

- **Production flashing**: USB-BOOTSEL → drag-and-drop UF2 onto
  the `RPI-RP2` mass-storage volume. No SWD probe required, no
  bootloader chain to maintain.
- **Bring-up / development**: 5-pin debug header carries SWDIO,
  SWCLK, GND and the UART1 stdio pair (GP4/GP5). A standard
  picoprobe / J-Link clone on the SWD pair, plus any 3.3 V serial
  adapter on the UART pair, gets you full RTT-style debug.
- The firmware never needs the SWD pair to enumerate USB —
  `tud_init()` runs unconditionally — so you can attach the
  debugger after the host has bound the CDC and watch the boot
  sequence retroactively.

## Pre-production gates

Two items remain open from the run-up to first production hardware
(`firmware/TODO.md` carries the full list):

- **udev rules + umockdev recording refresh** — the udev rules
  already match the production VID `0545:0101`; the umockdev
  fixture under `driver/tests/umockdev/` still carries
  pre-production traffic. Refresh requires a recording session
  against real rev-B hardware.
- **bcdDevice** — closed without a bump per the convention
  documented in `usb_descriptors.c`. Bumps only when the RPBP
  wire-protocol crosses a major version (RPBP v1 → v2), so
  Windows hosts don't have to re-pair on every firmware patch.

## Hardware files

The KiCad 9 source files for rev-B will land under
[`hardware/`](https://github.com/bauer-group/EMB-USBMultiBusBridge/tree/main/hardware).
Schematic, PCB layout and Gerber/drill outputs go there. **Status
today: source files pending** — the spec on this page is
authoritative for the board behaviour the firmware ships against.
