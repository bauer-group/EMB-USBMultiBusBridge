# Hardware — pinout reference

Default GPIO-to-function mapping for **rev-B** (RP2350B / QFN-80).
Authoritative C definitions live in
[`firmware/boards/rpbridge_rp2354b.h`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/firmware/boards/rpbridge_rp2354b.h).
Firmware reclaims pins via the
[pin broker](../firmware/resource-budget.md#5-gpio-ownership) at
runtime; treat this table as the **boot-time** assignment, not a
hard-wired mapping. For the full silicon-capability + reconfigurable
view see [pin-matrix.md](pin-matrix.md).

| GPIO | Default function           | Destination                                         |
|------|----------------------------|-----------------------------------------------------|
| GP0  | UART0 TX                   | Phy bank (RS232 / RS485 / TTL via on-board ICs)     |
| GP1  | UART0 RX                   | Phy bank                                            |
| GP2  | I²C0 SDA                   | Qwiic 1+2 (`J3`) and I²C0 4-pin header              |
| GP3  | I²C0 SCL                   | Qwiic 1+2 (`J3`) and I²C0 4-pin header              |
| GP4  | UART1 TX (debug stdio)     | 5-pin debug header                                  |
| GP5  | UART1 RX (debug stdio)     | 5-pin debug header                                  |
| GP6  | RS485 DE (PIO-timed)       | SN65HVD72 DE/RE                                     |
| GP7  | RS485 EN                   | SN65HVD72 shutdown                                  |
| GP8  | RS232 /SHDN                | MAX3232 shutdown                                    |
| GP9  | (free — user GPIO/PIO)     | PMOD spare                                          |
| GP10 | SPI1 SCK                   | SPI1 breakout (`J9`)                                |
| GP11 | SPI1 MOSI                  | SPI1 breakout (`J9`)                                |
| GP12 | SPI1 MISO                  | SPI1 breakout (`J9`)                                |
| GP13 | (broker-arbitrated CS)     | SPI1 breakout (`J9`) — userland-managed CS line     |
| GP14 | (broker-arbitrated CS)     | SPI1 breakout (`J9`) — userland-managed CS line     |
| GP15 | (broker-arbitrated CS)     | SPI1 breakout (`J9`) — userland-managed CS line     |
| GP16 | SPI0 MISO                  | PMOD (`J8`)                                         |
| GP17 | SPI0 CS                    | PMOD (`J8`)                                         |
| GP18 | SPI0 SCK                   | PMOD (`J8`)                                         |
| GP19 | SPI0 MOSI                  | PMOD (`J8`)                                         |
| GP20 | CAN TX (PIO0 / can2040)    | SN65HVD230 TXD                                      |
| GP21 | CAN RX (PIO0 / can2040)    | SN65HVD230 RXD                                      |
| GP22 | RGB LED red (sink, PWM)    | discrete RGB LED, common-anode                      |
| GP23 | RGB LED green (sink, PWM)  |                                                     |
| GP24 | RGB LED blue (sink, PWM)   |                                                     |
| GP25 | User button                | front-panel push button                             |
| GP26 | ADC0                       | user header "A0"                                    |
| GP27 | ADC1                       | user header "A1"                                    |
| GP28 | ADC2                       | user header "A2"                                    |
| GP29 | ADC3                       | user header "A3"                                    |
| GP30 | GPIO / PWM (user)          | 2×10 user header                                    |
| GP31 | GPIO / PWM (user)          | 2×10 user header                                    |
| GP32 | 1-Wire data (PIO1 SM 2)    | 1-Wire 3-pin header                                 |
| GP33 | PIO-UART #2 TX             | UART2 TTL header                                    |
| GP34 | I²C1 SDA                   | Qwiic 3 and I²C1 header                             |
| GP35 | I²C1 SCL                   | Qwiic 3 and I²C1 header                             |
| GP36 | PIO-UART #2 RX             | UART2 TTL header                                    |
| GP37 | GPIO / PWM (user)          | 2×10 user header                                    |
| GP38 | GPIO / PWM (user)          | 2×10 user header                                    |
| GP39 | GPIO / PWM (user)          | 2×10 user header                                    |
| GP40 | GPIO / PWM (user)          | 2×10 user header                                    |
| GP41 | I²S BCLK (PIO2)            | I²S 5-pin header                                    |
| GP42 | I²S LRCLK (PIO2)           | I²S 5-pin header (`= BCLK_PIN + 1`, side-set)       |
| GP43 | WS2812 / SK6812 data (PIO2)| LED-strip 3-pin header                              |
| GP44 | DMX-512 TX (PIO2)          | DMX 5-pin XLR header (user wires RS-485 transceiver)|
| GP45 | I²S DOUT (PIO2)            | I²S 5-pin header                                    |
| GP46 | VBUS sense                 | 100 kΩ / 49.9 kΩ divider from VBUS                  |
| GP47 | reserve / test pad         | testpad                                             |

**Header inventory (rev-B):**

| Connector | Function                                  | Notes                                    |
|-----------|-------------------------------------------|------------------------------------------|
| `J3`      | Qwiic 1+2 + 4-pin header (I²C0)           | three Qwiic JST-SH on the same I²C0 bus  |
| `J5`      | Qwiic 3 + 4-pin header (I²C1)             | independent I²C1, 80-pin variant only    |
| `J8`      | PMOD 2A (SPI0)                            | standard PMOD pinout                     |
| `J9`      | SPI1 breakout                             | SCK/MOSI/MISO + 3 user-CS pins           |
| —         | DE-9 RS232 (UART0 via MAX3232)            | CDC-ACM #0                               |
| —         | Phoenix MSTBA RS485 (UART0 via SN65HVD72) | CDC-ACM #1, mechanical 120 Ω jumper      |
| —         | UART2 TTL header                          | CDC-ACM #3 (PIO-UART)                    |
| —         | DE-9 CAN (CiA 303-1 pinout, SN65HVD230)   | mechanical 120 Ω jumper, gs_usb on host  |
| —         | DMX 5-pin XLR (TX only)                   | user-wired RS-485 transceiver            |
| —         | LED-strip 3-pin header                    | WS2812 / SK6812 (24 bits/pixel today)    |
| —         | I²S 5-pin header                          | TX only; BCLK/LRCLK consecutive          |
| —         | 1-Wire 3-pin header                       | external 4.7 kΩ pull-up to +3V3          |
| `J12`     | 2×10 user GPIO/PWM/ADC header             | GP30/31/37-40 + ADC0..3                  |
| —         | 5-pin debug header                        | SWDIO + SWCLK + GND + UART1 TX/RX (debug)|

## PIO allocation (current firmware)

| PIO block | Owner(s)                                                  |
|-----------|-----------------------------------------------------------|
| PIO0      | **`can2040`** — block-exclusive (32/32 instructions used) |
| PIO1      | PIO-UART #2 (TX + RX, ~12 instr) + 1-Wire (SM 2, ~14 instr) |
| PIO2      | WS2812 (SM 0, ~4 instr) + DMX TX (SM 1, ~5 instr) + I²S TX (SM 2, ~8 instr) |

For the live SM/instruction-budget tally see
[`docs/firmware/resource-budget.md`](../firmware/resource-budget.md#3-pio-blocks-state-machines-instructions).

## USB descriptor layout

Composite device with **base 4 CDC-ACMs + 2 vendor interfaces**
(rev-B with all transceiver phys assembled). The base count drops
automatically on bare-board variants — see the
[CDC-count formula in the rev-B board header](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/firmware/boards/rpbridge_rp2354b.h)
(`RPBRIDGE_BASE_CDC_COUNT`).

| TUSB CDC index | Role                                   | Bound by                                     |
|---------------:|----------------------------------------|----------------------------------------------|
| 0              | UART1 RS232 (MAX3232)                  | `cdc_acm` (Linux), `usbser.sys` (Windows)    |
| 1              | UART1 RS485 (SN65HVD72, auto DE)       | `cdc_acm` / `usbser.sys`                     |
| 2              | UART1 TTL (direct 3.3 V on UART0 hw)   | `cdc_acm` / `usbser.sys`                     |
| 3              | UART2 TTL (PIO-UART #2)                | `cdc_acm` / `usbser.sys`                     |
| 4 *(opt.)*     | RPBridge Debug Log (printf sink)       | only when `-DRPBRIDGE_DEBUG_CDC=ON`          |

Vendor interfaces (always present):

| iInterface              | Subclass    | Bound by                               |
|-------------------------|-------------|----------------------------------------|
| `RPBridge RPBP transport` | `0xFF/0x52` ('R') | `rpbridge.ko` / WinUSB via MS OS 2.0 |
| `RPBridge CAN (gs_usb)` | `0xFF/0x43` ('C') | mainline `gs_usb.ko` / `candle_api`  |

VID/PID = `0545:0101` (production, BAUER GROUP). Full descriptor
contract in [`docs/usb-vid-pid.md`](../usb-vid-pid.md).
