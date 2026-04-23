# Tutorial — CAN 2.0B with SocketCAN

RPBridge exposes CAN via a `gs_usb`-compatible vendor interface. The
mainline `drivers/net/can/usb/gs_usb.ko` driver binds it automatically
on Linux ≥ 5.14, so this tutorial assumes nothing RPBridge-specific
beyond the device being plugged in.

## 1. Confirm the netdev

```sh
ip -br link show type can
# can0             DOWN           ...
```

## 2. Configure bitrate and bring up

```sh
sudo ip link set can0 type can bitrate 500000 sample-point 0.875
sudo ip link set can0 up
ip -br link show can0
# can0             UNKNOWN        ...
```

## 3. Transmit and receive

```sh
candump can0 &
cansend can0 123#DEADBEEFCAFE0042
# can0  123   [7]  DE AD BE EF CA FE 00
```

## 4. Generate and dump traffic

```sh
cangen can0 -g 20 -n 500 -I i -L r &    # 500 frames, 20 ms apart
candump -ta can0 > capture.log
```

## 5. Stats and error counters

```sh
ip -s -s link show can0
# RX errors, TX errors, bus-off counters, etc.
```

## 6. Supported bitrates

| Bitrate   | Sample point | Notes                                   |
|-----------|--------------|-----------------------------------------|
| 125 kbit/s| 87.5 %       | Long-link-friendly.                     |
| 250 kbit/s| 87.5 %       | Industrial default.                     |
| 500 kbit/s| 80 %         | Automotive default.                     |
| 1 Mbit/s  | 75 %         | Requires clean wiring + short stubs.    |

CAN-FD is **not** supported in rev-B. can2040 is a classic CAN 2.0B
controller; a future hardware revision could add an external
MCP2518FD over SPI if FD becomes a requirement.
