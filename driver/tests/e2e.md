# End-to-end hardware verification checklist

This checklist is run by a human with real RPBridge rev-B hardware
plugged into a Linux host. It is the sign-off gate before cutting a
release. Each box is a command to run + an expected observation.

CI cannot run this document. Every line here needs physical
peripherals (I²C / SPI / GPIO loop-back jumpers, a CAN bus partner,
a logic analyser for PWM observation).

## Prerequisites

- [ ] Linux host running kernel 6.13+ with `CONFIG_RUST=y` and the
      driver built for it (`cd driver && make`).
- [ ] `rpbridge.ko` built, `gs_usb` present in-tree.
- [ ] udev rules from `driver/udev/` installed (gives non-root
      access to the CDCs via the `dialout` group).
- [ ] User is in groups `dialout`, `plugdev`, `i2c`, `gpio`, `spi`.
- [ ] Board rev-B plugged in. `lsusb` shows `1209:FFFE` (or the
      allocated PID) with 4 × CDC + 2 × vendor interfaces.

## 1. Device attach

- [ ] `sudo modprobe -v rpbridge`
      → dmesg shows `rpbridge: probing USB interface` and
        `rpbridge: spawned 6 aux_device children`.
- [ ] `ls /sys/bus/auxiliary/devices/ | grep rpbridge-`
      → lists `gpio.0 i2c.0 i2c.1 iio.0 pwm.0 spi.0 spi.1`.
- [ ] `dmesg | grep -E 'usbser|cdc_acm'` shows 4 CDC-ACM interfaces
      bound.
- [ ] `ip link` shows `can0` (from gs_usb binding).

## 2. UART — driverless path (ADR-0006)

Loop back UART TX to RX with a wire before these tests.

- [ ] `stty -F /dev/rpbridge-uart1-rs232 115200 raw -echo`
- [ ] `picocom -b 115200 /dev/rpbridge-uart1-rs232 &`
- [ ] Type `ABCDE` → host echoes it back (DTR triggers RS232 phy).
- [ ] Close picocom. DTR drops. Run
      `picocom -b 115200 /dev/rpbridge-uart1-rs485`.
- [ ] Type `ABCDE` again → host echoes it back AND a scope on the
      RS485 DE pin shows the pulse aligned with TX.
- [ ] Check `ls -l /dev/rpbridge-uart2`
      → exists, framing is 8N1 only (capability flag
        `uart.pio.8n1-only` in `dmesg | grep -i capability`).

## 3. CAN — SocketCAN via gs_usb

External CAN partner (peak PCAN-USB, SYS TEC, cannelloni loop) at
500 kbit/s.

- [ ] `sudo ip link set can0 up type can bitrate 500000`
      → no errors; `ip -d link show can0` shows `state ERROR-ACTIVE`.
- [ ] `candump -tz can0 &`, then on the partner send a frame with
      ID `0x123` and payload `DE AD BE EF`.
- [ ] candump prints `can0  123   [4]  DE AD BE EF` with a sub-ms
      timestamp.
- [ ] `cansend can0 456#CAFEBABE` → partner receives
      `0x456 / CA FE BA BE`.
- [ ] HW_TIMESTAMP smoke test:
      `sudo ip link set can0 up type can bitrate 500000 \
            hwts-enable 1` (if iproute2 supports it) OR
      `candump -H can0` — timestamps should be monotonically
      increasing in 1-µs resolution, not the 1-ms granularity
      of software timestamping. **Closes M10a verification.**

## 4. I²C — Qwiic sensor read

Plug a Qwiic device (e.g. SHT31 at `0x44`) onto Qwiic 1.

- [ ] `sudo i2cdetect -y 3` (bus idx 0 on our device is `/dev/i2c-3`
      on most hosts) — the expected address shows as `44`.
- [ ] `sudo i2cget -y 3 0x44 0x30A2 w`  (SHT31 status register read
      with repeated-start)
      → returns a non-zero 16-bit value; firmware log shows XFER
        with `FLAG_NO_STOP` set. **Validates M10f's
        write-restart-read coalescing.**

## 5. SPI — MCP23S17 or similar

Jumper MOSI to MISO on the SPI-0 PMOD pins (loop-back).

- [ ] `spi-pipe -d /dev/spidev0.0 -s 1000000 -b 8 <<< 'Hello'`
      → stdout is exactly `Hello` (full-duplex echo).
- [ ] `dmesg -c` shows no SPI errors.

## 6. GPIO — chip + edge events

- [ ] `gpiodetect` lists `rpbridge-gpio.0` with N lines (matching
      the firmware's 11-pin whitelist GP30-31 + GP37-45).
- [ ] `gpioset rpbridge-gpio.0 30=1` → scope shows GP30 goes high.
- [ ] `gpioget rpbridge-gpio.0 31` (jumpered to GND) → `0`.
- [ ] `gpiomon -r -b rpbridge-gpio.0 31` — toggle GP31 with a wire
      → each edge prints within <5 ms. **Validates M10e WATCH event
        pump latency.**

## 7. PWM — scope observation

- [ ] `echo 0 > /sys/class/pwm/pwmchip0/export`
- [ ] `echo 1000000 > /sys/class/pwm/pwmchip0/pwm0/period`
- [ ] `echo  500000 > /sys/class/pwm/pwmchip0/pwm0/duty_cycle`
- [ ] `echo 1       > /sys/class/pwm/pwmchip0/pwm0/enable`
      → scope shows 1 kHz square wave at 50 % duty on the PMW-
        capable pin (GP30 by default).
- [ ] `echo  50000 > /sys/class/pwm/pwmchip0/pwm0/duty_cycle`
      → duty smoothly drops to 5 %.

## 8. ADC / IIO — single-shot + streaming

- [ ] `cat /sys/bus/iio/devices/iio:device0/in_voltage0_raw`
      → 0-4095 value matching the voltage on GP26.
- [ ] `cat /sys/bus/iio/devices/iio:device0/in_temp_input`
      → a plausible cdegC (20000 to 35000 in an office).
- [ ] Streaming:
      `iio_readdev -b 1024 -s 10240 iio:device0 > dump.bin`
      → writes 10k samples. `xxd dump.bin | head` shows LE u16
        samples at ~1 kHz (default rate).  **Validates M10i
        triggered-buffer path.**

## 9. Simultaneous CDC + driver (ADR-0005 coexistence)

While `iio_readdev` above is still running, in another terminal:

- [ ] `picocom -b 115200 /dev/rpbridge-uart1-rs232` → works; bytes
      flow through UART0 without interrupting the IIO stream.
- [ ] `dmesg` shows no "interface reset" or "usbser disconnect"
      messages. **Validates ADR-0005 single-configuration model.**

## 10. Unplug / replug

- [ ] Unplug the device while running `candump -i can0` and
      `iio_readdev`.
- [ ] dmesg shows clean disconnect (`rpbridge: interface
      disconnected` + aux_device unregister for each child). No
      use-after-free spam or Oops.
- [ ] Replug. Everything rebinds automatically within 2 s.

---

## Sign-off

Fill in when all boxes above check:

- Tester: ____________________
- Date:   ____________________
- Firmware SHA: ____________________
- Driver SHA:   ____________________
- Kernel version: ____________________

Observations / regressions:

```
(free-form notes)
```
