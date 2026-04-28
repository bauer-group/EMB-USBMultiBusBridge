# Hardware — bring-up procedure & log

The procedure to take a freshly-fabricated rev-B board from "just
came back from the SMT line" to "validated, can hit a desk and start
shipping units off it." Run every check on every new board before
declaring it shippable; the procedure surfaces solder defects, BOM
swaps and firmware/hardware mismatches early.

The page also serves as the running engineering log — each board
that goes through bring-up appends one entry to the log table at the
bottom.

> **Time budget:** ~45 minutes per board on the bench, including the
> first power-on, USB enumeration, every bus loopback, RGB
> calibration and the entry write-up.

## Pre-power-on (DMM only, ~10 min)

Before connecting USB **at all** — these checks catch the kind of
solder bridge that destroys an LDO in 2 ms.

1. **Visual inspection** under 10× loupe:
   - QFN-80 RP2354B fillets even on all four sides
   - No bridges between USB-C VBUS / D+ / D- / GND
   - LDO IC orientation matches silkscreen (pin-1 dot)
   - Electrolytics polarity correct
   - No solder balls on the QSPI flash side

2. **DMM continuity** (USB cable disconnected, no power):

   | Probe between        | Expected           | If wrong              |
   | -------------------- | ------------------ | --------------------- |
   | VBUS pad ↔ GND       | open (≫ 100 kΩ)    | short — stop, find it |
   | +3V3 net ↔ GND       | open (≫ 100 kΩ)    | short — LDO output    |
   | RP2350 VDD_CORE ↔ GND| open (≫ 1 MΩ)      | short — die-side bridge |
   | USB D+ ↔ D-          | open (≫ 1 MΩ)      | bridge across pair    |
   | RP2350 VDD_IO pins ↔ each other | shorted (same rail) | one trace open  |

3. **Mechanical jumpers** are seated in their factory positions
   (CAN 120 Ω closed, RS485 120 Ω closed).

If any pre-power check fails → **stop, fix, re-run pre-power.** Do
not power up a board with a known short to ground on a power rail.

## First power-on (~5 min)

Cable: USB-C from a current-limited supply or a host that doesn't
mind a brown-out. Reasonable bench setup is a USB-C power meter
inline so you can read VBUS and current draw at the same time.

```text
Plug in →  measure within first second:

  VBUS        4.85 – 5.15 V       (USB-C nominal, no PD)
  IBUS_in     50 – 120 mA         (boot + USB enumeration burst)

After the firmware boots (~150 ms):

  +3V3        3.27 – 3.33 V       (LDO output, idle)
  IBUS_in     35 – 80 mA          (steady state, LED dimmed)
  LDO surface 25 – 35 °C          (room-temp + 5–10 °C, no bus load)
```

If `IBUS_in > 200 mA` at idle → unplug immediately, recheck for
short / wrong LDO / wrong RP2350 brick.

If `+3V3` reads outside 3.27–3.33 V → either the LDO is wrong, the
sense divider is wrong, or VBUS is sagging (try a different cable).

The RGB LED should run a heartbeat scene shortly after boot
(slow pulse on green during the watchdog grace window). If it
stays dark → SWD attach to find out where the firmware crashed.

## SWD attach + serial stdio (~5 min, optional)

Skip this section if power-on + USB enumeration both work the
first time — most boards do, and the SWD step is for diagnosing
boards that don't.

Tools: any picoprobe-class debugger on the 5-pin debug header
(SWDIO + SWCLK + GND), plus any 3.3 V serial adapter on the same
header's UART1 stdio pair (GP4/GP5 = TX/RX).

```sh
# OpenOCD via picoprobe at /dev/ttyACM0:
openocd -f interface/cmsis-dap.cfg -c 'adapter speed 5000' \
        -f target/rp2350.cfg

# In a second terminal — serial console for printf() output:
picocom -b 115200 /dev/ttyUSB0
```

The firmware's `printf()` output names every subsystem as it inits;
a halted board reveals where it stopped.

## USB enumeration check (~2 min)

```sh
lsusb -d 0545:0101 -v | head -40
```

Expected highlights:

```text
idVendor           0x0545 BAUER GROUP
idProduct          0x0101
bcdDevice            1.00
iManufacturer       1 BAUER GROUP
iProduct            2 RPBridge
iSerialNumber       3 RPB-<16 hex>
bNumConfigurations  1
```

`dmesg | tail` should show `cdc_acm` binding the four CDC interfaces
plus the `gs_usb` binding the CAN interface (no `rpbridge.ko` on the
host yet — that's the next step).

If a CDC fails to enumerate → check the per-interface descriptor
strings via `lsusb -v`; if descriptors look fine, the firmware
shipped wrong USB-side endpoint allocation.

## Driver bind verification (~3 min)

Install the kernel module (see [`build.md`](../driver/build.md))
and replug:

```sh
sudo modprobe rpbridge
sudo udevadm trigger
dmesg | tail -20
```

Expected dmesg entries (in order):

```text
rpbridge 1-x:1.4: probing matched USB interface
rpbridge 1-x:1.4: capability map: 0x... bytes
rpbridge_gpio rpbridge.gpio.0: registered (32 lines)
rpbridge_i2c rpbridge.i2c.0: registered as i2c-N
rpbridge_i2c rpbridge.i2c.1: registered as i2c-N
rpbridge_spi rpbridge.spi.0: registered as spidevN.M
…
```

The auxiliary children spawn in capability-map order — use the
same map to verify all expected subsystems registered.

## Per-bus smoke tests (~15 min, the bulk of bring-up)

Each test exercises one signal path end-to-end. Run them in this
order — earlier tests rule out classes of failure (USB-stack,
power, MCU itself) before you blame later results on a single
bus.

### LED + button (no test gear required)

```sh
rpbridge-ctl led set --rgb FF0000          # solid red
rpbridge-ctl led set --rgb 00FF00          # solid green
rpbridge-ctl led set --rgb 0000FF          # solid blue
rpbridge-ctl led set --rgb FFFFFF          # full white
```

While the LED is on white, **note the brightness** of each channel
visually — they should look balanced. Wide imbalance (e.g. green
overpowering red and blue) means the per-channel calibration in
`firmware/src/main.c` (`RPBRIDGE_LED_CAL_R/G/B`) needs adjustment
for this board's LED batch.

```sh
# button:
rpbridge-ctl gpio --bus 0 watch 25         # press the front-panel button
                                            # — should print "GP25 = 0"
                                            # while held, "GP25 = 1" released
```

### I²C buses (Qwiic + 4-pin headers)

Plug a known-good I²C device (BME280, AHT20, anything with a fixed
address) into Qwiic 1 (bus 0) and run:

```sh
i2cdetect -y 0
# expected: device address visible at the documented row/col
```

Repeat for bus 1 (Qwiic 3 connector). On rev-B both buses should
respond; on a Pico 2 variant only bus 0 exists.

### SPI loopback

Wire MOSI ↔ MISO on the PMOD (J8) — no external device needed:

```sh
spidev_test -D /dev/spidev0.0 -s 1000000 -p 0x12,0x34,0x56,0x78
# expected RX: 12 34 56 78  (loopback echo)
```

Repeat with `-s 24000000` to confirm the high-speed path.

### CAN

Connect the CAN port to a CANable / candle-bus reference adapter,
both with 120 Ω terminators in (jumpers closed on the rev-B board).

```sh
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
candump can0 &
cansend can0 123#DEADBEEF                  # send from the bridge
                                            # CANable should echo it
```

Frames should appear on both ends. Bus errors (`error-active`
status in `ip -details link show can0`) usually mean a mismatched
or absent terminator on one end.

### UART transceivers (rev-B only)

```sh
# RS232 — loopback DE-9 pin 2 ↔ 3:
picocom -b 115200 /dev/ttyACM0
# type chars; you should see them echoed.

# RS485 — loopback A ↔ B with 120 Ω across, or use a USB-RS485 dongle:
picocom -b 115200 /dev/ttyACM1
```

For each phy bank, the firmware's auto-DTR-switch routes UART0 HW
to whichever CDC currently has DTR=1. `picocom` raises DTR by
default → opening the device picks that bank.

### ADC

```sh
# four channels A0..A3 — connect a known voltage (e.g. divider on +3V3):
iio_info | grep -A 4 'rpbridge'

# or one-shot read:
rpbridge-ctl adc read 0
rpbridge-ctl adc read 1
```

Compare the returned value to a DMM measurement on the same pin —
within ±2 % at room temperature.

### PIO applets (1-Wire, WS2812, DMX, I²S)

These need specific external hardware:

- **1-Wire**: DS18B20 sensor on the 1-Wire header with the 4.7 kΩ
  pull-up. `cat /sys/bus/w1/devices/28-*/temperature`.
- **WS2812**: 8-pixel strip on the LED-strip header.
  `rpbridge-ctl led strip set --rgb 00FF00`.
- **DMX**: external RS-485 transceiver (MAX485) wired to the
  DMX XLR, into a DMX fixture (LED par, fog machine).
  `rpbridge-ctl dmx set 1 255` (channel 1 → full).
- **I²S**: an I²S DAC (UDA1334 / PCM5102) on the I²S header.
  Pipe a sine wave: see the `rpbridge-ctl i2s` subcommand.

## Final checks (~5 min)

1. **Hot-soak**: leave the board running for 30 min with the CAN bus
   echoing traffic and an I²C scan loop. LDO surface should stay
   below 60 °C, IBUS_in shouldn't drift more than 10 mA.
2. **Hot-replug**: yank the USB cable, plug it into a different
   port, confirm enumeration recovers without dmesg errors.
3. **Sleep / wake**: suspend the host, wait 30 s, resume. The
   device should re-enumerate cleanly. (Some hosts don't VBUS-cycle
   on resume — that's a host quirk, not a board defect.)
4. **`rpbridge-ctl info` snapshot**: capture for the log entry
   below.

## Bring-up log

Each row corresponds to one board. Fill from top to bottom; entries
that don't apply (no I²S DAC on the bench today) get `n/a`.

### Template

```text
## YYYY-MM-DD — RPB-<serial>

| Check                   | Result                              |
| ----------------------- | ----------------------------------- |
| Pre-power continuity    | clean / detail                      |
| IBUS_in idle            | … mA                                |
| +3V3 idle               | … V                                 |
| LDO surface 30 min soak | … °C                                |
| USB enumeration         | clean / dmesg detail                |
| Driver bind             | all subsystems / list missing       |
| LED RGB calibration     | R=…% G=…% B=…% for neutral white    |
| Button GP25             | press / release events seen         |
| I²C bus 0 (Qwiic 1)     | device at 0x… / "no devices"        |
| I²C bus 1 (Qwiic 3)     | device at 0x… / "no devices"        |
| SPI loopback @ 1 MHz    | OK / mismatch                       |
| SPI loopback @ 24 MHz   | OK / mismatch                       |
| CAN echo @ 500 kbit/s   | both directions / one-way / none    |
| RS232 loopback          | OK / no echo / framing errors       |
| RS485 loopback          | OK / no echo / DE-timing issue      |
| UART2 PIO loopback      | OK / no echo                        |
| ADC channel sweep       | within ±… % of DMM                  |
| 1-Wire DS18B20          | … °C / not detected                 |
| WS2812 strip            | colours render / pixel offset / n/a |
| DMX fixture             | level changes / n/a                 |
| I²S DAC                 | tone audible / silent / n/a         |
| Hot-replug              | recovers / errors                   |
| Suspend/resume          | recovers / errors                   |
| Issues observed         | …                                   |
| Changes planned         | …                                   |
```

### Entries

_No entries yet — the first rev-B run is pending. Append above
with a copy of the template, one section per board._
