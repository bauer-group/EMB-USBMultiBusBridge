# Hardware — pinout reference

Default GPIO-to-function mapping for rev-B. Firmware reclaims pins
via the pin broker at runtime; treat this table as the **boot-time
assignment**, not a hard-wired mapping.

| GPIO | Default function           | Destination                                         |
|------|----------------------------|-----------------------------------------------------|
| GP0  | UART0 TX                   | Phy bank (RS232 / RS485 / TTL)                      |
| GP1  | UART0 RX                   | Phy bank                                            |
| GP2  | I²C0 SDA                   | Qwiic 1+2 and I²C0 header                           |
| GP3  | I²C0 SCL                   | Qwiic 1+2 and I²C0 header                           |
| GP4  | UART1 TX (debug)           | 5-pin debug header                                  |
| GP5  | UART1 RX (debug)           | 5-pin debug header                                  |
| GP6  | RS485 DE (PIO-timed)       | SN65HVD72 DE/RE                                     |
| GP7  | RS485 EN                   | SN65HVD72 shutdown                                  |
| GP8  | RS232 /SHDN                | MAX3232 shutdown                                    |
| GP9  | SPI0 INT                   | PMOD SPI0                                           |
| GP10 | SPI1 SCK                   | SPI1 header                                         |
| GP11 | SPI1 MOSI                  | SPI1 header                                         |
| GP12 | SPI1 MISO                  | SPI1 header                                         |
| GP13 | SPI1 CS0                   | SPI1 header                                         |
| GP14 | SPI1 CS1                   | SPI1 header                                         |
| GP15 | SPI1 CS2                   | SPI1 header                                         |
| GP16 | SPI0 MISO                  | PMOD                                                |
| GP17 | SPI0 CS                    | PMOD                                                |
| GP18 | SPI0 SCK                   | PMOD                                                |
| GP19 | SPI0 MOSI                  | PMOD                                                |
| GP20 | CAN TX (PIO0 / can2040)    | SN65HVD230 TXD                                      |
| GP21 | CAN RX (PIO0 / can2040)    | SN65HVD230 RXD                                      |
| GP22 | RGB LED red (sink)         | discrete RGB LED                                    |
| GP23 | RGB LED green (sink)       |                                                     |
| GP24 | RGB LED blue (sink)        |                                                     |
| GP25 | User button                | front-panel push button                             |
| GP26 | ADC0                       | user header "A0"                                    |
| GP27 | ADC1                       | user header "A1"                                    |
| GP28 | ADC2                       | user header "A2"                                    |
| GP29 | ADC3                       | user header "A3"                                    |
| GP30 | GPIO / PWM #1              | user header                                         |
| GP31 | GPIO / PWM #2              | user header                                         |
| GP32 | 1-Wire (PIO2)              | 1-Wire 3-pin header                                 |
| GP33 | PIO-UART #2 TX             | UART header #2                                      |
| GP34 | I²C1 SDA                   | Qwiic 3 and I²C1 header                             |
| GP35 | I²C1 SCL                   | Qwiic 3 and I²C1 header                             |
| GP36 | PIO-UART #2 RX             | UART header #2                                      |
| GP37–GP45 | GPIO / PWM              | 2×10 user header                                    |
| GP46 | VBUS sense                 | 100 kΩ / 49.9 kΩ divider from VBUS                  |
| GP47 | Reserve / test pad         | testpad                                             |

## PIO allocation

| PIO block | SMs used | Purpose                                           |
|-----------|---------:|---------------------------------------------------|
| PIO0      | 3 / 4    | **can2040 (CAN 2.0B)** — block reserved exclusively. |
| PIO1      | 2–4 / 4  | Additional PIO-UARTs.                             |
| PIO2      | 2–4 / 4  | RS485-DE precision timing, 1-Wire, spare applets. |
