# RPBP v1 — I²C subsystem

**Subsystem id: `0x01`.** Controls the two on-chip I²C master
instances the RPBridge exposes on its three Qwiic connectors plus
two 4-pin breakout headers. Unlike UART, **I²C has no driverless
fallback** — the host library or driver always talks to the
subsystem over RPBP.

Bus map (rev-B):

| `idx` | SDA pin | SCL pin | Connectors                   |
|-------|--------:|--------:|------------------------------|
| `0`   | GP2     | GP3     | Qwiic 1 + Qwiic 2 + I²C0 4-pin header |
| `1`   | GP34    | GP35    | Qwiic 3 + I²C1 4-pin header  |

On-board pull-ups: 2.2 kΩ to 3.3 V on both SDA and SCL (sized for
Fast-mode 400 kHz cable lengths up to ~50 cm). The firmware also
keeps the internal pull-ups enabled so a disconnected Qwiic cable
leaves the bus in a defined idle state; do not rely on the
internal pulls alone for timing.

## Opcode summary

| Opcode | Name        | Purpose                                                    |
|-------:|-------------|------------------------------------------------------------|
| `0x00` | `PROBE`     | Single-address ACK check — `i2cdetect -q` equivalent.      |
| `0x01` | `XFER`      | One transaction: write, read, or write-restart-read.       |
| `0x02` | `SCAN`      | Sweep all 128 7-bit addresses and return an ACK bitmap.    |
| `0x03` | `SET_FREQ`  | Change bus clock (100 k / 400 k / 1 M).                    |
| `0x04` | `GET_FREQ`  | Read current bus clock.                                    |

Opcodes `0x05..0x7F` are reserved for additive minor-version
extensions (10-bit addressing, SMBus block transactions, target
mode, PEC).

Supported baud rates: **100 kHz (Standard-mode)**, **400 kHz
(Fast-mode)**, **1 MHz (Fast-mode Plus)**. Arbitrary values are
refused with `EINVAL`.

---

## `0x00 — PROBE`

Presence check for a single slave. Issues a 1-byte read; the
address byte is either ACK'd (slave present, success) or NACK'd
(slave absent).

**Request (binary):**

```text
 0        1        2        3
 +--------+--------+--------+--------+
 |  0x01  |  0x00  |  bus   |  addr  |
 +--------+--------+--------+--------+
```

- `bus` (u8) — 0 or 1.
- `addr` (u8) — 7-bit slave address, 0x00..0x7F.

**Response (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x01  |  0x00  | status |
 +--------+--------+--------+
```

- `status = OK (0)` — slave ACK'd, device present.
- `status = ENODEV (4)` — slave NACK'd the address byte.
- `status = ETIMEDOUT (6)` — slave held SCL low past the probe
  timeout (1 ms). Usually indicates a stuck / mis-wired device.
- `status = EINVAL (2)` / `EMSGSIZE (7)` — malformed request.

## `0x01 — XFER`

Single I²C transaction. The opcode automatically chooses the
right START/RESTART/STOP pattern based on `tx_len` and `rx_len`:

| tx_len | rx_len | Pattern                                           |
|:------:|:------:|---------------------------------------------------|
| > 0    | 0      | START, write, STOP (or skip STOP if `NO_STOP`)    |
| 0      | > 0    | START, read, STOP                                  |
| > 0    | > 0    | START, write, repeated-START, read, STOP (canonical register-access pattern) |

**Request (binary):**

```text
 0        1        2        3        4
 +--------+--------+--------+--------+--------+
 |  0x01  |  0x01  |  bus   |  addr  | flags  |
 +--------+--------+--------+--------+--------+
 |   tx_len (LE u16)       |   rx_len (LE u16)         |
 +--------+-----------------+--------+--------+-----------------+
 |                         tx_data (tx_len bytes)               |
 +--------------------------------------------------------------+
```

- `flags` — only bit 0 (`NO_STOP`) is defined; all other bits MUST
  be zero (`EINVAL` otherwise). `NO_STOP` suppresses the closing
  STOP so the next request can issue a repeated-START — rarely
  needed at this layer because the write-restart-read case is
  already the default.
- `tx_len` / `rx_len` — each bounded by 2048 bytes.

**Response (binary):**

```text
 0        1        2        3
 +--------+--------+--------+-----------------+
 |  0x01  |  0x01  | status |   rx_len (LE u16) |
 +--------+--------+--------+-----------------+
 |             rx_data (rx_len bytes if status == OK)           |
 +--------------------------------------------------------------+
```

- `status = OK (0)` — transaction complete; `rx_data` is valid.
- `status = ENODEV (4)` — slave did not acknowledge the address.
- `status = EIO (5)` — bus-level error (arbitration loss, bit error).
- `status = ETIMEDOUT (6)` — clock stretched past 100 ms.
- `status = EMSGSIZE (7)` — tx or rx length exceeds 2048.

When `status != OK`, the response `rx_len` field is 0 and no
`rx_data` follows.

## `0x02 — SCAN`

Sweep all 128 7-bit addresses on a bus and return a compact
ACK bitmap. Equivalent to `i2cdetect -y <bus>` on Linux.

**Request (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x01  |  0x02  |  bus   |
 +--------+--------+--------+
```

**Response (binary):**

```text
 0        1        2        3       18
 +--------+--------+--------+--------+
 |  0x01  |  0x02  |   OK   | bitmap[16] |
 +--------+--------+--------+------------+
```

- `bitmap` is exactly 16 bytes. Bit `(addr & 7)` in byte
  `(addr >> 3)` is set when address `addr` ACK'd. Bytes are
  little-endian inside themselves (bit 0 = address 0, bit 7 =
  address 7, …).

Reserved-address bits (0x00–0x07, 0x78–0x7F) are probed like any
other; hosts can mask them out if they want an i2cdetect-exact
answer. Scan completes in < 200 ms on a silent bus.

## `0x03 — SET_FREQ`

Reconfigure the bus clock without touching pins.

**Request (binary):**

```text
 0        1        2        3
 +--------+--------+--------+-----------------+
 |  0x01  |  0x03  |  bus   |  freq_hz (LE u32) |
 +--------+--------+--------+-----------------+
```

**Response (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x01  |  0x03  | status |
 +--------+--------+--------+
```

- `status = OK (0)` — new frequency accepted.
- `status = EINVAL (2)` — `freq_hz` is not one of 100 000, 400 000,
  1 000 000.

## `0x04 — GET_FREQ`

Read back the current bus clock.

**Request (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x01  |  0x04  |  bus   |
 +--------+--------+--------+
```

**Response (binary):**

```text
 0        1        2        3
 +--------+--------+--------+-----------------+
 |  0x01  |  0x04  |   OK   |  freq_hz (LE u32) |
 +--------+--------+--------+-----------------+
```

`status = ENOENT (4)` for an unknown / uninitialised `bus`.

## Capability-map advertising

Each HW bus appears under `buses.i2c` in the capability map as:

- `idx` (uint) — instance index (0 or 1).
- `sda_pin` / `scl_pin` (uint) — the GP* pad number the bus lives on.
- `max_freq` (uint) — 1 000 000 Hz on rev-B.
- `freqs` (array of uint) — supported clock ladder.

Feature flags `"i2c.100k"`, `"i2c.400k"`, `"i2c.1m"` appear in
the top-level `features` array so hosts can gate speed selection
cheaply without walking the bus array.

## Userland expectations (Linux-first path)

Once the Rust kernel driver exposes `rpbridge-i2c` auxiliary
children (M9 / M2-driver), each bus shows up as `/dev/i2c-N` and
works with the full i2c-tools suite unmodified:

```sh
i2cdetect -y N           # SCAN opcode under the hood
i2cget    -y N 0x48      # XFER with tx=[reg], rx=1
i2cset    -y N 0x48 reg  # XFER write-only
i2ctransfer              # driven through XFER + NO_STOP
```

In the driverless path, `rpbridge-ctl` exposes the same opcodes
directly (`rpbridge-ctl i2c scan 0`, `rpbridge-ctl i2c xfer …`).
