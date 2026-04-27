# Hardware — pin capability matrix

Per-pin reference for **what every GPIO can do on each supported
board**. Use this page when you need to:

- decide whether a pin is suitable for a given subsystem (`hispeed counter`,
  `ADC`, `PWM`, `I²C`, `SPI`, …)
- understand why the firmware places a function on a specific pin
- plan a custom board variant or a wiring change

The "Default function" column shows what the firmware ships with on
power-on. Anything else in "Reconfigurable to" is reachable at runtime
**provided** the corresponding subsystem is enabled in the active
board header (`RPBRIDGE_HAVE_*`) — the
[pin broker](../firmware/resource-budget.md#5-gpio-ownership) refuses
overlapping claims, so a re-assignment frees the previous owner first.

> **Silicon vs. board.** This page is the **board layer** — the
> physical connector, the assigned default, and the firmware-policy
> view of "is this pin user-exposed?". Below each table, the
> [#Silicon capability](#silicon-capability-rp2350a-vs-rp2350b)
> section captures what the RP2350 datasheet allows on each pin
> regardless of board. A function can land on any pin where (silicon
> permits) ∧ (board exposes the pin) ∧ (no broker conflict).

---

## Glossary

| Tag         | Means                                                                           |
|:------------|:--------------------------------------------------------------------------------|
| `IO`        | General-purpose digital I/O via SIO. Always available on every GP pin.          |
| `PWM`       | RP2350 PWM-slice channel. Each pin maps to a fixed `(slice, channel)` pair.     |
| `ADC`       | SAR-ADC analog input. Only GP26..GP29 (and the on-die temp-sensor input).       |
| `I²C0/1`    | Hardware I²C IP block. Multiple pin options per signal (see silicon table).     |
| `SPI0/1`    | Hardware SPI (PL022) IP block. Multiple pin options per signal.                 |
| `UART0/1`   | Hardware UART IP block. Multiple TX/RX pin options.                             |
| `PIO0/1/2`  | PIO state-machine pin. Any GP can be claimed by any PIO program (within block). |
| `cdc:rs232` | Pin is dedicated to one of the CDC-ACM-driven UART transceiver phys.            |
| `header:Jx` | Pin is brought out on connector / header `Jx` (rev-B board only).               |
| `int:N`     | Internal-only — wired to an on-board peripheral, not on a user header.          |

---

## RPBridge rev-B (`rpbridge_rp2354b`, RP2350B QFN-80)

48 GPIO total. ADC channels A0..A3 = GP26..GP29. PWM slice mapping
follows `(gpio >> 1) & 0xF` for the slice and `gpio & 1` for the
channel (A=0, B=1).

| GPIO | Default function       | Header / destination          | Reconfigurable to                            |
|-----:|:-----------------------|:------------------------------|:---------------------------------------------|
| GP0  | UART0 TX               | int — phy bank                | `IO`, `PIO0/1/2`                             |
| GP1  | UART0 RX               | int — phy bank                | `IO`, `PIO`                                   |
| GP2  | I²C0 SDA               | Qwiic 1+2, J3 header          | `IO`, `PWM 1A`, `PIO`                         |
| GP3  | I²C0 SCL               | Qwiic 1+2, J3 header          | `IO`, `PWM 1B`, `PIO`                         |
| GP4  | UART1 TX (debug)       | 5-pin debug header            | `IO`, `PIO` *(would lose stdio)*              |
| GP5  | UART1 RX (debug)       | 5-pin debug header            | `IO`, `PIO` *(would lose stdio)*              |
| GP6  | RS485 DE (PIO-timed)   | int — SN65HVD72               | `IO`, `PIO0/1/2`                              |
| GP7  | RS485 EN (shutdown)    | int — SN65HVD72               | `IO`                                          |
| GP8  | RS232 /SHDN            | int — MAX3232                 | `IO`                                          |
| GP9  | (free, PMOD spare)     | PMOD                          | `IO`, `PWM 4B`, `PIO`                         |
| GP10 | SPI1 SCK               | SPI1 header                   | `IO`, `PWM 5A`, `PIO`                         |
| GP11 | SPI1 MOSI              | SPI1 header                   | `IO`, `PWM 5B`, `PIO`                         |
| GP12 | SPI1 MISO              | SPI1 header                   | `IO`, `PWM 6A`, `PIO`                         |
| GP13 | (user-managed CS)      | SPI1 header                   | `IO`, `PWM 6B`, `PIO`                         |
| GP14 | (user-managed CS)      | SPI1 header                   | `IO`, `PWM 7A`, `PIO`                         |
| GP15 | (user-managed CS)      | SPI1 header                   | `IO`, `PWM 7B`, `PIO`                         |
| GP16 | SPI0 MISO              | PMOD                          | `IO`, `PWM 0A`, `PIO`, `UART0 TX`             |
| GP17 | SPI0 CS                | PMOD                          | `IO`, `PWM 0B`, `PIO`, `UART0 RX`             |
| GP18 | SPI0 SCK               | PMOD                          | `IO`, `PWM 1A`, `PIO`                         |
| GP19 | SPI0 MOSI              | PMOD                          | `IO`, `PWM 1B`, `PIO`                         |
| GP20 | CAN TX (`can2040`)     | int — SN65HVD230              | `IO`, `PIO0` (already), `UART1 TX`            |
| GP21 | CAN RX (`can2040`)     | int — SN65HVD230              | `IO`, `PIO0`, `UART1 RX`                      |
| GP22 | RGB LED red            | int — discrete RGB            | `IO`, `PWM 3A` *(currently used for LED)*     |
| GP23 | RGB LED green          | int — discrete RGB            | `IO`, `PWM 3B`                                |
| GP24 | RGB LED blue           | int — discrete RGB            | `IO`, `PWM 4A`                                |
| GP25 | User button            | front-panel push button       | `IO`, `PWM 4B`                                |
| GP26 | **ADC0 (A0)**          | A0 user header                | `IO`, `PWM 5A`, `I²C1 SDA` *(if ADC freed)*   |
| GP27 | **ADC1 (A1)**          | A1 user header                | `IO`, `PWM 5B`, `I²C1 SCL` *(if ADC freed)*   |
| GP28 | **ADC2 (A2)**          | A2 user header                | `IO`, `PWM 6A`, `I²C0 SDA` *(if ADC freed)*   |
| GP29 | **ADC3 (A3)**          | A3 user header                | `IO`, `PWM 6B`, `I²C0 SCL` *(if ADC freed)*   |
| GP30 | user GPIO/PWM #1       | 2×10 user header              | `IO`, `PWM 7A`, `PIO`, `I²C1 SDA`             |
| GP31 | user GPIO/PWM #2       | 2×10 user header              | `IO`, `PWM 7B`, `PIO`, `I²C1 SCL`             |
| GP32 | **1-Wire data**        | 1-Wire header                 | `IO`, `PIO1`                                  |
| GP33 | PIO-UART #2 TX         | UART2 TTL header              | `IO`, `PIO1`                                  |
| GP34 | I²C1 SDA               | Qwiic 3 header                | `IO`, `PWM 1A`, `PIO`                         |
| GP35 | I²C1 SCL               | Qwiic 3 header                | `IO`, `PWM 1B`, `PIO`                         |
| GP36 | PIO-UART #2 RX         | UART2 TTL header              | `IO`, `PIO1`                                  |
| GP37 | user GPIO/PWM #3       | 2×10 user header              | `IO`, `PWM 2B`, `PIO`                         |
| GP38 | user GPIO/PWM #4       | 2×10 user header              | `IO`, `PWM 3A`, `PIO`                         |
| GP39 | user GPIO/PWM #5       | 2×10 user header              | `IO`, `PWM 3B`, `PIO`                         |
| GP40 | user GPIO/PWM #6       | 2×10 user header              | `IO`, `PWM 4A`, `PIO`                         |
| GP41 | **I²S BCLK**           | I²S header                    | `IO`, `PWM 4B`, `PIO2`                        |
| GP42 | **I²S LRCLK**          | I²S header (must be BCLK+1)   | `IO`, `PWM 5A`, `PIO2`                        |
| GP43 | user GPIO #7           | 2×10 user header              | `IO`, `PWM 5B`, `PIO`                         |
| GP44 | **DMX-512 TX**         | DMX 5-pin XLR header          | `IO`, `PWM 6A`, `PIO2`                        |
| GP45 | **I²S DOUT**           | I²S header                    | `IO`, `PWM 6B`, `PIO2`                        |
| GP46 | VBUS sense             | int — 100k/49k9 divider       | `ADC` (if rerouted), `IO`                     |
| GP47 | reserved               | —                             | `IO`, `PIO`                                   |

**Header map (rev-B):**
- `J3` Qwiic 1+2 (I²C0) · `J5` Qwiic 3 (I²C1) · `J8` PMOD (SPI0)
- `J9` SPI1 breakout · `J10..J11` UART headers · `J12` 2×10 user-GPIO/PWM/ADC
- 5-pin debug header · DMX XLR · I²S 5-pin

---

## RPBridge Pi Pico 2 (`rpbridge_pico2`, RP2350A QFN-60)

30 GPIO total (GP0..GP29). Same ADC channel mapping as rev-B
(GP26..GP29). GP25 is the on-board green LED.

There is no on-board phy bank — RS232 / RS485 require an external
transceiver wired by the user; the firmware ships
`RPBRIDGE_HAVE_RS232_PHY` and `RPBRIDGE_HAVE_RS485_PHY` **off** by
default. This collapses the USB descriptor from 4 CDC-ACMs to 2
(TTL on UART0 hardware + UART2 PIO-UART). I²C bus 1 and SPI bus 1
also drop out (no QFN-60 pads); see
`firmware/boards/rpbridge_pico2.h` for the authoritative HAVE-flag set.

| GPIO | Default function          | Destination                     | Reconfigurable to                                      |
|-----:|:--------------------------|:--------------------------------|:--------------------------------------------------------|
| GP0  | UART0 TX (TTL)            | user header                     | `IO`, `PIO`                                              |
| GP1  | UART0 RX (TTL)            | user header                     | `IO`, `PIO`                                              |
| GP2  | I²C0 SDA                  | user header                     | `IO`, `PWM 1A`, `PIO`                                    |
| GP3  | I²C0 SCL                  | user header                     | `IO`, `PWM 1B`, `PIO`                                    |
| GP4  | UART1 TX (debug stdio)    | pin pad                         | `IO`, `PIO`                                              |
| GP5  | UART1 RX (debug stdio)    | pin pad                         | `IO`, `PIO`                                              |
| GP6  | **PIO-UART #2 TX**        | user header                     | `IO`, `PIO`                                              |
| GP7  | **PIO-UART #2 RX**        | user header                     | `IO`, `PIO`                                              |
| GP8  | I²S BCLK *(PIO2 SM 2)*    | user header (broker-shared)     | `IO`, `PWM 4A`, `PIO`                                    |
| GP9  | I²S LRCLK *(PIO2 SM 2)*   | user header (`= BCLK + 1`)      | `IO`, `PWM 4B`, `PIO`                                    |
| GP10 | user GPIO/PWM             | user header                     | `IO`, `PWM 5A`, `PIO`                                    |
| GP11 | user GPIO/PWM             | user header                     | `IO`, `PWM 5B`, `PIO`                                    |
| GP12 | 1-Wire data *(PIO1 SM 2)* | user header (broker-shared)     | `IO`, `PWM 6A`, `PIO`                                    |
| GP13 | I²S DOUT *(PIO2 SM 2)*    | user header                     | `IO`, `PWM 6B`, `PIO`                                    |
| GP14 | WS2812 data *(PIO2 SM 0)* | user header                     | `IO`, `PWM 7A`, `PIO`                                    |
| GP15 | DMX-512 TX *(PIO2 SM 1)*  | user header                     | `IO`, `PWM 7B`, `PIO`                                    |
| GP16 | SPI0 MISO                 | user header                     | `IO`, `PWM 0A`, `PIO`, `UART0 TX`                        |
| GP17 | SPI0 CS                   | user header                     | `IO`, `PWM 0B`, `PIO`, `UART0 RX`                        |
| GP18 | SPI0 SCK                  | user header                     | `IO`, `PWM 1A`, `PIO`                                    |
| GP19 | SPI0 MOSI                 | user header                     | `IO`, `PWM 1B`, `PIO`                                    |
| GP20 | CAN TX (`can2040`)        | user header                     | `IO`, `PIO0`                                             |
| GP21 | CAN RX (`can2040`)        | user header                     | `IO`, `PIO0`                                             |
| GP22 | reserved                  | —                               | `IO`, `PWM 3A`, `PIO`                                    |
| GP23 | reserved                  | —                               | `IO`, `PWM 3B`, `PIO`                                    |
| GP24 | VBUS sense                | int — VSYS divider              | `ADC` (if rerouted), `IO`                                |
| GP25 | **status LED (green)**    | on-board LED                    | `IO`, `PWM 4B` *(currently used for LED)*                |
| GP26 | **ADC0 (A0)**             | user header                     | `IO`, `PWM 5A`                                           |
| GP27 | **ADC1 (A1)**             | user header                     | `IO`, `PWM 5B`                                           |
| GP28 | **ADC2 (A2)**             | user header                     | `IO`, `PWM 6A`                                           |
| GP29 | **ADC3 (A3)**             | user header                     | `IO`, `PWM 6B`                                           |

**Pico 2 specifics:**

- The rev-B RGB LED collapses to the **single green LED on GP25**.
  `RPBRIDGE_HAVE_RGB_LED=0` selects the mono-LED code path in
  `firmware/src/main.c`: brightness = max(r,g,b) folded through
  the green calibration.
- The PIO applets (1-Wire, WS2812, DMX, I²S) share the user-GPIO
  whitelist (GP8..GP15). The pin broker rejects overlapping claims
  at runtime, so enabling I²S + WS2812 + 1-Wire + DMX simultaneously
  occupies exactly the 6 pins listed above and leaves GP10/GP11
  free for plain GPIO/PWM use.
- `RPBRIDGE_HAVE_I2C1` and `RPBRIDGE_HAVE_SPI1` are both `0` —
  the QFN-60 package doesn't expose the GP-numbers those buses
  would need.

---

## Silicon capability (RP2350A vs. RP2350B)

The RP2350 IO matrix lets each GP carry one of a fixed set of
hardware functions chosen via the IO-MUX. The pattern is regular:

| Function         | GP-number selector (RP2350)                                           |
|:-----------------|:----------------------------------------------------------------------|
| `IO` (SIO)       | every GP                                                              |
| `I²C0 SDA`       | GP `n` where `n mod 4 == 0` *(GP0, 4, 8, 12, 16, 20, 24, 28, …)*      |
| `I²C0 SCL`       | GP `n mod 4 == 1`                                                     |
| `I²C1 SDA`       | GP `n mod 4 == 2`                                                     |
| `I²C1 SCL`       | GP `n mod 4 == 3`                                                     |
| `SPI0 SCK`       | GP `n mod 8 == 2 or 18`                                               |
| `SPI0 RX`        | GP `n mod 8 == 0 or 16`                                               |
| `SPI0 TX`        | GP `n mod 8 == 3 or 19`                                               |
| `SPI0 CSn`       | GP `n mod 8 == 1 or 17`                                               |
| `SPI1 SCK`       | GP `n mod 8 == 10`                                                    |
| `UART0 TX/RX`    | GP{0/1, 12/13, 16/17, 28/29} (B: also 32/33, 44/45)                  |
| `UART1 TX/RX`    | GP{4/5, 8/9, 20/21, 24/25} (B: also 36/37, 40/41)                    |
| `PWM slice s`    | slice `s = (gp >> 1) & 0x0F`, channel A on even GP, B on odd GP       |
| `ADC ch 0..3`    | GP26..GP29 only — both A and B parts                                  |
| `PIO0/1/2`       | every GP (programmable pin-set)                                       |

**A vs. B differences:**
- RP2350A: 30 GPIOs (GP0..29) on QFN-60 package.
- RP2350B: 48 GPIOs (GP0..47) on QFN-80; PWM slice 8..11 only fully
  exposed on the B die.
- Both: 4 ADC channels routed to GP26..29; channel 4 = on-die temp.

The RP2350B "extra" pins (GP30..47) reuse the regular 4-pin mod
pattern — e.g. GP32 acts as I²C0 SDA, GP44 as I²C0 SDA, etc. The
firmware exploits this on rev-B by keeping I²C0 on GP2/GP3 (header)
**and** allowing user-GPIO claims on GP32/44 because both pins are
broker-arbitrated.

---

## High-speed counter / pulse-counter feasibility

The user-facing question "can I configure a pin as a high-speed
pulse counter?" is **yes, with PIO**. RP2350 has no dedicated
counter peripheral; the canonical recipe is a 16-instruction PIO
program that counts edges on a single input pin and pushes the
total into the RX FIFO every M cycles.

Currently this is **planned but not landed** — see
[ADR-0009 #counter](../adr/0009-pin-function-flexibility.md#high-speed-counter)
for the protocol-side design. The PIO budget already reserves a
slot in PIO2 for it (see
[`docs/firmware/resource-budget.md`](../firmware/resource-budget.md#pending-applets)).

---

## Cross-references

- [`firmware/boards/rpbridge_rp2354b.h`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/firmware/boards/rpbridge_rp2354b.h)
  — authoritative C definitions for rev-B.
- [`firmware/boards/rpbridge_pico2.h`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/firmware/boards/rpbridge_pico2.h)
  — authoritative C definitions for the Pico 2 variant.
- [Firmware resource budget](../firmware/resource-budget.md) — PIO,
  DMA, USB endpoint and PWM-slice ledger.
- [ADR-0008 — multi-board variants](../adr/0008-multi-board-variants.md)
  — why we have two board headers.
- [ADR-0009 — pin function flexibility](../adr/0009-pin-function-flexibility.md)
  — protocol-side capability discovery + reassignment opcodes.
