# Driver — subsystem integration

Every RPBridge capability surfaces through the **canonical** Linux
subsystem for its class. No private character devices for bus
access.

## GPIO

- Driver: `rpbridge-gpio` aux child registers a `struct gpio_chip`
  with `nr` lines taken from the capability map.
- IRQs: nested `irq_chip`, GPIO cdev **v2** only
  (`GPIO_V2_LINE_*` ioctls). sysfs GPIO is **not** provided —
  deprecated since 4.8.
- Tools: `libgpiod 2.x` (`gpioget`, `gpioset`, `gpiomon`).

```sh
gpiodetect | grep rpbridge
gpioinfo $(gpiodetect | awk '/rpbridge/ {print $1}')
gpiomon -r -f $(gpiodetect | awk '/rpbridge/ {print $1}') 7
```

## I²C

- Driver: `rpbridge-i2c` aux child registers an `i2c_adapter` with
  `I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL`.
- Tools: `i2c-tools` (`i2cdetect`, `i2cget`, `i2cset`,
  `i2ctransfer`), any kernel-side I²C client driver.

```sh
i2cdetect -y 7
i2cget  -y 7 0x76 0xD0        # read BME280 id register
i2cset  -y 7 0x76 0xF4 0x27   # set BME280 ctrl_meas
```

## SPI

- Driver: `rpbridge-spi` aux child registers an `spi_controller`.
- Tools: `spidev` via `/dev/spidevN.M`, `flashrom`, any kernel-side
  SPI client driver.

```sh
flashrom -p linux_spi:dev=/dev/spidev0.0,spispeed=4000 -r dump.bin
```

## PWM

- Driver: `rpbridge-pwm` aux child registers a `pwm_chip`.
- Tools: sysfs PWM API under `/sys/class/pwm/pwmchipN`,
  `pwm-ctrl`, or kernel consumer drivers like `pwm-beeper`.

```sh
echo 0    > /sys/class/pwm/pwmchip0/export
echo 1000000 > /sys/class/pwm/pwmchip0/pwm0/period
echo  500000 > /sys/class/pwm/pwmchip0/pwm0/duty_cycle
echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable
```

## IIO (ADC)

- Driver: `rpbridge-iio` aux child registers an `iio_dev` with four
  channels mapped to GP26..GP29.
- Tools: `iio_info`, `iio_readdev`, `libiio`, Grafana's IIO plugin.

```sh
iio_info | grep rpbridge -A4
iio_readdev -b 256 iio:device0 voltage0 > samples.raw
```

## CAN

CAN is **not** carried by this module. The RPBridge firmware exposes
a `gs_usb`-compatible vendor interface on the same composite device;
mainline `drivers/net/can/usb/gs_usb.ko` binds it directly and
presents a standard SocketCAN network device.

```sh
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
candump can0
```

## UART

UART is also **not** carried by this module. Three CDC-ACM pairs on
the composite device present as `/dev/ttyACMN`; mainline `usbser.ko`
handles them. The RPBridge driver does, however, handle the UART
*phy* selection (RS232 / RS485 / TTL) via an RPBP control command —
call it through `rpbridge-ctl set-phy`.
