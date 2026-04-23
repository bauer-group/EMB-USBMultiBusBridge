# Tutorial — GPIO with libgpiod

`libgpiod 2.x` with the cdev-v2 ABI is the supported Linux API for
GPIO. The old `/sys/class/gpio` sysfs interface is deprecated and
RPBridge does not provide it.

## Find the GPIO chip

```sh
gpiodetect
# gpiochip0  [rpbridge-gpio]  (14 lines)
```

Store the name in a variable for the rest of the examples:

```sh
CHIP=$(gpiodetect | awk '/rpbridge-gpio/ {print $1}')
```

## Inspect lines

```sh
gpioinfo "$CHIP"
# gpiochip0 - 14 lines:
#     line  0:  unnamed  input  active-high
#     line  1:  unnamed  input  active-high
#     ...
#     line  7:  unnamed  input  active-high
```

## Drive an output

```sh
gpioset --chip "$CHIP" 30=1
sleep 0.2
gpioset --chip "$CHIP" 30=0
```

## Read an input

```sh
gpioget --chip "$CHIP" 7
# 1
```

## Wait for an edge

```sh
gpiomon --chip "$CHIP" --rising-edge --falling-edge 7
# event:  FALLING EDGE offset: 7 timestamp: [1745324923.812 341 000]
# event:  RISING  EDGE offset: 7 timestamp: [1745324924.019 002 000]
```

Timestamps come from the firmware's device-boot clock, mapped onto
`CLOCK_MONOTONIC_RAW` by the driver via the RPBP `TIME_SYNC` path.
Accuracy is ±200 µs on a warm USB link.

## Bias and drive configuration

```sh
gpioset --chip "$CHIP" --bias=pull-up --drive=open-source 30=1
```

All RP2350 pad features (pull up/down, open-drain, slew rate, drive
strength) are mapped to the generic `gpiod_line_request_config_flags`.
