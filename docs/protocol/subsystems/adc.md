# ADC subsystem (subsys = 5)

Single-shot reads from the RP2350 SAR ADC **and** continuous
sampling streams via RPBP dynamic channels. Both paths share the
same channel map; streaming runs on Core 1 (`io_control`) and uses
the RPBP credit-flow-control feature.

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
ferrite bead on the +3V3 rail (see hardware design #2.1).

Conversion: `mV = raw * 3300 / 4095`.

| Opcode | Name            | Description                                        |
|:------:|:----------------|:---------------------------------------------------|
| `0x00` | `READ`          | Single-shot read, returns raw code + mV.           |
| `0x01` | `READ_MANY`     | Read every channel whose bit is set in a mask.     |
| `0x02` | `GET_REF`       | Read VREF in mV.                                   |
| `0x03` | `TEMP_READ`     | Decoded temperature in 0.01 °C units.              |
| `0x04` | `STREAM_START`  | Open a continuous-sampling stream on a dyn channel.|
| `0x05` | `STREAM_STOP`   | Close a previously-opened stream.                  |

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

## 0x04 — STREAM_START

**Args** (8 B):

```text
[0..1]   channel           LE u16   dynamic channel 16..239
[2]      channel_mask      u8       bits 0..4 select ADC channels
[3]      reserved          u8       MUST be 0
[4..7]   rate_hz           LE u32   samples/sec, clamped to 1..10000
```

**Reply body** (2 B): `[actual_channel LE u16]` — echoes the channel
the device accepted. `status = EBUSY` if the channel was already in
use; `EINVAL` for bad mask / channel out of range.

On success the device immediately starts Core-1 ADC sampling and
begins emitting `STREAM_DATA` frames on the selected channel.
Initial per-stream credit is 8192 bytes; hosts top up via
`STREAM_CREDIT` frames on the same channel (see the RPBP spec's
credit-fc feature).

### STREAM_DATA payload layout

Each emitted frame carries one acquisition tick:

```text
[0..3]       timestamp_us     LE u32   device boot-relative us
[4]          channel_mask     u8       copied from the STREAM_START arg
[5]          sample_count     u8       popcount(channel_mask)
[6..6+2n-1]  samples          LE u16 × sample_count
```

Samples appear in ascending channel-index order. The stream's own
16-bit RPBP header `seq` field lets hosts detect dropped frames.

## 0x05 — STREAM_STOP

**Args** (2 B): `[channel LE u16]`. Tears down Core-1 sampling and
closes the RPBP stream. Drains any in-flight samples silently.
`status = ENOENT` if the channel was not open.

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
`/sys/bus/iio/devices/iio:deviceX`. Continuous sampling uses the
standard IIO triggered-buffer pattern: the kernel driver opens an
RPBP stream with `ADC_STREAM_START`, points
`/dev/iio:deviceX` at the per-device ring, and feeds host credit
via `STREAM_CREDIT` as userland consumes frames. On close the
driver sends `ADC_STREAM_STOP` and stops issuing credit.
