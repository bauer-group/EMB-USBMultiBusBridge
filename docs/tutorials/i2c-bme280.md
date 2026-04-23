# Tutorial — reading a BME280 over I²C

Prerequisites: RPBridge plugged in, driver loaded, BME280 on a Qwiic
cable.

## 1. Confirm the adapter

```sh
i2cdetect -l | grep rpbridge
# → i2c-7  i2c  rpbridge-i2c  I²C adapter
```

## 2. Scan the bus

```sh
i2cdetect -y 7
#      0  1  2  ...  75 76 77
# 00:    -- -- ...
# 70: -- -- -- -- -- -- 76 --     ← BME280 answering on 0x76
```

## 3. Attach the kernel client driver

```sh
sudo modprobe bmp280_i2c
echo bme280 0x76 | sudo tee /sys/bus/i2c/devices/i2c-7/new_device
```

The IIO device `iio:device0` appears.

## 4. Read temperature, humidity, pressure

```sh
cat /sys/bus/iio/devices/iio:device0/in_temp_input
# 24310       ← 24.310 °C

cat /sys/bus/iio/devices/iio:device0/in_humidityrelative_input
# 42350       ← 42.35 % RH

cat /sys/bus/iio/devices/iio:device0/in_pressure_input
# 1013250     ← 101.325 kPa
```

## 5. Automate with `iio_readdev`

```sh
iio_readdev -b 256 iio:device0 temp humidityrelative pressure > samples.raw
```

Pipe into Grafana's IIO plugin for a live dashboard.

## Device-tree auto-provisioning

If the RPBridge dongle has the BME280 fixed on its own PCB (not a
pluggable Qwiic accessory), the firmware can ship a pre-baked DT
overlay that the kernel driver applies at bind time — the
`new_device` step disappears. See [`dts/overlays/`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/tree/main/dts/overlays)
for examples.
