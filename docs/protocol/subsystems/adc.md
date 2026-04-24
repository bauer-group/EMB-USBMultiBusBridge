# ADC subsystem (subsys = 5)

Single-shot reads from the RP2350 SAR ADC. Streaming / IIO mode
(continuous sampling on core 1) lands with M6; the opcode space is
already reserved.

**Channel map (rev-B)**

| Channel | GPIO | Header label | Source                        |
|:-------:|:----:|:-------------|:------------------------------|
|   0     | GP26 | A0           | external (2×10 user header)   |
|   1     | GP27 | A1           | external                      |
|   2     | GP28 | A2           | external                      |
|   3     | GP29 | A3           | external                      |
|   4     |  —   | —            | on-chip temperature sensor    |

Resolution: 12 bits (`raw ∈ [0, 4095]`).
Reference voltage: `VREF = ADC_AVDD = 3300 mV` — filtered through a
ferrite bead on the +3V3 rail (see hardware design §2.1).

Conversion: `mV = raw * 3300 / 4095`.

| Opcode | Name         | Description                                     |
|:------:|:-------------|:------------------------------------------------|
| `0x00` | `READ`       | Single-shot read, returns raw code + mV.        |
| `0x01` | `READ_MANY`  | Read every channel whose bit is set in a mask.  |
| `0x02` | `GET_REF`    | Read VREF in mV.                                |
| `0x03` | `TEMP_READ`  | Decoded temperature in 0.01 °C units.           |

## 0x00 — READ

**Args** (1 B): `[channel]`.

**Reply body** (4 B): `[raw LE u16][mv LE u16]`.

Returns `ENOENT` for channels >= 4 that aren't the temperature
sensor (channel 4). For channel 4 the `mv` field is the raw ADC
reading converted to mV (NOT decoded °C — use `TEMP_READ` for that).

## 0x01 — READ_MANY

**Args** (2 B): `[channel_mask LE u16]`. Bit 0 = channel 0, …, bit 4
= temperature sensor.

**Reply body**:

```text
[0]            count         u8     (number of channels read)
[1 + i*5 + 0]  channel_id    u8
[1 + i*5 + 1]  raw           LE u16
[1 + i*5 + 3]  mv            LE u16
```

Channels are read in ascending index order.

## 0x02 — GET_REF

**Args**: none. **Reply body** (4 B): `[vref_mv LE u32]`.

## 0x03 — TEMP_READ

**Args**: none. **Reply body** (2 B): `[temp_cdegC LE i16]`.

Decoded via the RP2350 datasheet §12.4.5 calibration:

```text
T[°C] = 27 - (V_mV - 706) / 1.721
```

Returned value is centi-degrees (multiply by 0.01 to get °C). E.g.
2700 = 27.00 °C.

## Capability advertising

```text
buses.adc[0] = {
    idx: 0, vref_mv: 3300, resolution_bits: 12,
    channels: [
        { ch: 0, gp: 26, kind: "external" },
        …
        { ch: 4,         kind: "temperature" },
    ],
}
features[] += "adc.temp"
```

## Linux userland

Each external channel is exported as an IIO input channel under
`/sys/bus/iio/devices/iio:deviceX`. Once the M6 streaming channels
land, `/dev/iio:deviceX` becomes pollable for trigger-driven reads
without per-sample USB round-trips.
