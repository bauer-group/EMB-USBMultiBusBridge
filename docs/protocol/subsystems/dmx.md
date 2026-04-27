# RPBP — DMX subsystem (subsys = `0x08`)

DMX512-A transmitter driven by a PIO state machine on PIO2.
TX-only in v1; RX is a future extension once we wire up RDM (DMX
remote-device management) bidirectional support.

The PIO program emits the slot bytes at 250 kbaud 8N2; the C driver
generates the BREAK + Mark-After-Break (MAB) sequence by directly
driving the pin while the SM is paused. This split keeps the PIO
program tiny and the BREAK timing host-tunable.

Hardware: an external RS-485 transceiver (MAX485 / SN75176 /
SN65HVD12 …) MUST be wired between the GPIO and the XLR-5 connector.
The GPIO carries 3.3 V single-ended; the transceiver converts to
the differential pair DMX requires.

Access path: vendor RPBP. Linux has no in-kernel DMX subsystem;
OLA (Open Lighting Architecture) is the de-facto userspace stack.
A future char-device wrapper (driver/TODO.md #M12) will let OLA
bind to `/dev/rpbridge-dmxN`.

## Capabilities

`buses.dmx[*]` carries:

| key         | type   | meaning                                  |
|-------------|--------|------------------------------------------|
| `idx`       | uint   | universe index (always `0` in v1)       |
| `tx_pin`    | uint   | GPIO feeding the RS-485 transceiver      |
| `max_slots` | uint   | always `512` (DMX512-A spec ceiling)     |
| `direction` | text   | `"tx"` (v1)                              |

Feature flag: `dmx.tx`.

## Opcodes

### `0x00` GET_INFO

Request: empty.

Response (10 bytes):

```
+-+-+-+-+-+-+-+-+-+-+
|c|p|r|x|slt|brk|mab|
+-+-+-+-+-+-+-+-+-+-+
```

* `c` (u8) — universe count.
* `p` (u8) — GPIO carrying TX (before the transceiver).
* `r` (u8) — running flag (0/1).
* `x` (u8) — reserved.
* `slt` (LE u16) — currently configured slot count.
* `brk` (LE u16) — BREAK length in µs (≥ 88).
* `mab` (LE u16) — MAB length in µs (≥ 8).

### `0x01` SET_SLOTS

Request: `[idx u8][offset LE u16][bytes …]`. Copies bytes into the
universe buffer at slot `offset`. Slot 0 is the start code (typically
`0x00`); slots 1..512 are DMX channels.

Constraints:

* `offset + len` MUST NOT exceed 513 (start code + 512 channels).
* Total wire payload ≤ MTU (4 KiB).
* Updates land in the next emitted frame.

### `0x02` START

Request: `[idx u8]`. Begins continuous frame transmission at ~40 Hz.
Each frame uses the current slot buffer.

### `0x03` STOP

Request: `[idx u8]`. Halts transmission, leaves the line idle HIGH
so the transceiver disables cleanly.

### `0x04` SET_BREAK

Request: `[idx u8][break_us LE u16][mab_us LE u16]`. Adjust the
BREAK and MAB durations. Spec minima:

* `break_us` ≥ 88 µs (clamped on the device side; values < 88 are
  silently raised). `0` keeps the current value.
* `mab_us` ≥ 8 µs (clamped). `0` keeps the current value.

Useful for older receivers that need longer BREAKs.

## Frame timing

A full 513-slot frame at 250 kbaud is approximately:

```
BREAK (≥ 88 µs) + MAB (≥ 8 µs) + 513 × 44 µs ≈ 22.7 ms
```

The driver schedules one frame every 25 ms, giving ~40 Hz refresh
with a small scheduling margin. That matches USITT's recommended
floor and is what most lighting consoles expect.

## Examples

### Send a single channel value

```
GET_INFO              → running=0, slot_count=512
SET_SLOTS  idx=0, offset=1, bytes=[0xFF]    # ch1 = full
START      idx=0
```

### Stop the stream cleanly

```
STOP       idx=0
```

### Custom break for a finicky receiver

```
SET_BREAK  idx=0, break_us=176, mab_us=12   # double-spec break
```

## Error semantics

| Status     | Cause                                                 |
|------------|-------------------------------------------------------|
| `ENOENT`   | `idx` ≠ 0                                             |
| `EINVAL`   | offset/len out of range, slot_count out of range      |
| `EAGAIN`   | transmitter not initialised                           |
| `EBUSY`    | pin/PIO conflict at firmware boot                     |
| `ENOTSUP`  | board built without `RPBRIDGE_HAVE_DMX`               |

## Hardware wiring (typical)

```
RP2350  GP44 ───┬──────► DI (transceiver input)
                                │
        GND ────┼──────► GND
                                │
        +5V ────┼──────► VCC      (MAX485 needs 5 V; some
                                  3.3 V transceivers like
                                  SN65HVD12 work directly)
                                │
                          ┌─────┴─────┐
                          │  MAX485   │
                          └─────┬─────┘
                       A ──┬─── XLR-5 pin 3
                       B ──┴─── XLR-5 pin 2
                                  XLR-5 pin 1 = GND
```

DE/RE on the transceiver is tied HIGH (continuous transmit). For a
half-duplex setup, route DE to a GPIO and toggle it around frames —
not needed for v1's TX-only mode.
