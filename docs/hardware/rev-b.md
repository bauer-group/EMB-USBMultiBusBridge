# Hardware — rev-B

The current hardware revision is **rev-B**: a 2-layer, 80 × 55 mm PCB
built around the Raspberry Pi **RP2354B** (RP2350B die + 2 MB stacked
QSPI flash in QFN-80). Power is USB-C bus-powered only — no external
supply, no barrel jack, no PD negotiation.

The authoritative design document is
[`docs/drafts/rpbridge-hardware-design.md`](../drafts/rpbridge-hardware-design.md).
Highlights:

- **Single discrete RGB LED** on GP22/GP23/GP24 driven directly from
  GPIO, common-anode, one resistor per channel.
- **One canonical connector per bus**: DE-9 for CAN (CiA 303-1 pinout),
  DE-9 for RS232, Phoenix MSTBA for RS485, three Qwiic JST-SH for I²C,
  PMOD 2A for SPI, 2×10 header for GPIO/PWM/ADC.
- **Real mechanical jumpers** (not solder jumpers) for the CAN and
  RS485 120 Ω terminators — moved with fingernails.
- **5-pin debug header** (SWDIO / SWCLK / GND + debug UART TX/RX).
- **PTC-fused USB input** with Schottky reverse-feed block and
  TVS clamp.

BOM totals about **21 € per unit in singles, 11 € in 100s**.

## Hardware files

Once the rev-B KiCad project is committed, the schematic, PCB layout
and fabrication outputs live under [`hardware/`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/tree/main/hardware).
Current status: source files pending; the draft in `docs/drafts/` is
sufficient for a fab-ready prototype when ready.
