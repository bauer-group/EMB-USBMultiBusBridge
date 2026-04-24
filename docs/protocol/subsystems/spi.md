# SPI subsystem (subsys = 2)

The SPI subsystem exposes the two RP2350 PL022 instances — `SPI #0`
on the PMOD header and `SPI #1` on the breakout header with three
slave-select lines (CS0/CS1/CS2).

| Opcode | Name         | Description                                  |
|:------:|:-------------|:---------------------------------------------|
| `0x00` | `XFER`       | Full-duplex transfer with optional CS frame. |
| `0x01` | `SET_MODE`   | CPOL/CPHA (mode 0..3). LSB-first rejected.   |
| `0x02` | `SET_FREQ`   | Set the baud rate; returns actual applied.   |
| `0x03` | `GET_FREQ`   | Read back the active baud.                   |
| `0x04` | `CS_ASSERT`  | Drive a CS pin LOW (manual control).         |
| `0x05` | `CS_RELEASE` | Drive a CS pin HIGH.                         |

All opcodes carry binary arguments. The CBOR flag is ignored (v1).

## 0x00 — XFER

**Args** (8 B header + variable TX payload):

```text
[0]      instance      (0..1)
[1]      cs_pin        (GP number, 0xFF = no CS manipulation)
[2]      flags         (see RPBP_SPI_FLAG_*)
[3]      reserved      (MUST be 0)
[4..5]   tx_len        LE u16
[6..7]   rx_len        LE u16
[8..]    tx_data       tx_len bytes
```

Flags:

| Bit | Name      | Effect                                                         |
|:---:|:----------|:---------------------------------------------------------------|
|  0  | `HOLD_CS` | Leave CS asserted after the transfer for multi-op transactions |

**Wire behaviour**

On the wire the transfer is always a full-duplex burst of
`max(tx_len, rx_len)` frames. TX is zero-padded past `tx_len`; only
the first `rx_len` MISO bytes are returned to the host. CS is driven
LOW before the first byte and HIGH after the last (unless
`HOLD_CS=1`, or `cs_pin == 0xFF` for externally-managed CS).

**Reply body**: `[rx_len LE u16][rx_data]`.

**Status**: `OK` / `EINVAL` / `EMSGSIZE` / `EBUSY` (broker) / `EIO`.

## 0x01 — SET_MODE

**Args** (2 B): `[instance][mode_bits]`

Mode bits:

| Bit | Name         | Meaning                                   |
|:---:|:-------------|:------------------------------------------|
|  0  | `CPHA`       | 0 = sample on leading edge, 1 = trailing  |
|  1  | `CPOL`       | 0 = clock idle low, 1 = idle high         |
|  2  | `LSB_FIRST`  | Bit order — rejected with `ENOTSUP` (PL022 is MSB-only) |

Bits 3..7 MUST be zero. SPI modes 0..3 map to the canonical
`(CPOL, CPHA)` pairs.

## 0x02 — SET_FREQ

**Args** (5 B): `[instance][freq_hz LE u32]`.

**Reply body**: `[applied_hz LE u32]`. Hardware lands on discrete
divisors so the applied rate is typically a bit lower than requested.

## 0x03 — GET_FREQ

**Args** (1 B): `[instance]`. **Reply body**: `[freq_hz LE u32]`.

## 0x04 / 0x05 — CS_ASSERT / CS_RELEASE

**Args** (1 B): `[cs_pin]`. Claims the pin on first use
(`PIN_OWNER_SPI`, function `PIN_FUNC_SPI_CS`). Subsequent calls toggle
the level. Empty reply body; status reflects broker conflicts.

## Capability advertising

Each instance appears in `buses.spi` as:

```text
{ idx, sck_pin, mosi_pin, miso_pin, max_freq, cs_pins[...], int_pin? }
```

The top-level `features` array carries `"spi.mode-0-3"` so hosts can
gate behaviour without walking the bus map.

## Linux userland

The host-side driver maps each `(instance, cs_pin)` pair to a
`/dev/spidev<bus>.<cs>` device exposed by the `spidev` kernel module
(see `docs/driver/subsystems.md`). Transfers use the standard
`SPI_IOC_MESSAGE` ioctl; the Rust driver translates each message
entry into a single XFER frame plus an optional tail with `HOLD_CS=1`
when the `cs_change=0` flag is requested.
