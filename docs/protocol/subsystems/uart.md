# RPBP v1 — UART subsystem

**Subsystem id: `0x06`.** Controls the out-of-band aspects of each
UART — physical-layer selection, advertised capabilities, phy state.
The **byte stream itself does not flow through this subsystem**; it
uses CDC-ACM interfaces (0 and 1 of the composite device), so normal
tools (`minicom`, `picocom`, PuTTY, `System.IO.Ports.SerialPort`)
operate unchanged.

Instance map (rev-B):

| `uart_idx` | Backend        | CDC-ACM interface | Supported phys       |
|-----------:|----------------|-------------------|----------------------|
| `0`        | UART0 HW       | CDC #0            | TTL, RS232, RS485    |
| `1`        | PIO-UART #2    | CDC #1            | TTL only             |

## Opcode summary

| Opcode | Name       | Purpose                                                          |
|-------:|------------|------------------------------------------------------------------|
| `0x00` | `SET_PHY`  | Select the transceiver driving a phy-bank-capable UART instance. |
| `0x01` | `GET_PHY`  | Read back the current phy mode.                                  |

Opcodes `0x02..0x7F` are reserved for additive minor-version
extensions (break-timing, flow-control config, hardware loopback,
diagnostic counters).

## `0x00 — SET_PHY`

**Request (binary):**

```text
 0        1        2        3
 +--------+--------+--------+--------+
 |  0x06  |  0x00  | idx    |  phy   |
 +--------+--------+--------+--------+
```

- `idx` (u8) — UART instance index. See the table above.
- `phy` (u8) — desired phy mode:
  - `0` = **OFF** (transceivers tri-stated, line idle)
  - `1` = **TTL** (direct 3.3 V drive to the TTL header)
  - `2` = **RS232** (MAX3232 active, SN65HVD72 shut down)
  - `3` = **RS485** (SN65HVD72 active, MAX3232 shut down, DE pin
    toggled automatically around every TX burst)

**Response (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x06  |  0x00  | status |
 +--------+--------+--------+
```

- `status = OK (0)` on success.
- `status = ENOTSUP (10)` if `idx` refers to a TTL-only backend
  (PIO-UART #2, rev-B).
- `status = ENOENT (4)` for an unknown `idx`.
- `status = EINVAL (2)` for a `phy` value outside `0..3`.
- `status = EMSGSIZE (7)` if the request payload length isn't 4.

Transitions are safe to perform at any time. The firmware drops the
currently-active transceiver's enable pin before asserting the next
one, so at no instant are two transceivers fighting for the TX node.

## `0x01 — GET_PHY`

**Request (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x06  |  0x01  | idx    |
 +--------+--------+--------+
```

**Response (binary):**

```text
 0        1        2        3
 +--------+--------+--------+--------+
 |  0x06  |  0x01  |   OK   |  phy   |
 +--------+--------+--------+--------+
```

- `phy` (u8) matches the encoding above. For TTL-only backends
  (PIO-UART), the response is always `phy = 1` (TTL).

`status = ENOENT` for an unknown `idx`; `EMSGSIZE` for wrong request
length.

## RS485 DE timing

When `phy = RS485`, the firmware drives the DE pin (GP6 on rev-B)
HIGH just before the first bit of every TX burst enters the UART
shift register and releases it back to LOW once the hardware BUSY
flag clears — i.e. after the last stop bit has fully shifted out.

The scaffold uses a software state machine ticked from the main
loop (see `firmware/src/periph/uart_hw.c::uart_hw_tick`). A later
PIO applet may replace it for bit-precise timing at > 1 Mbaud, but
the current implementation is timing-correct at all supported baud
rates (up to 3 Mbaud) with a tick period ≤ 5 ms.

## Interaction with line coding

Line coding (baud, data bits, stop bits, parity) is set via the
standard CDC-ACM `SetLineCoding` class request on the respective
CDC interface — **not** through this subsystem. The firmware
forwards `SetLineCoding` directly to the underlying UART (HW or
PIO).

## Capability-map advertising

Each UART instance appears under `buses.uart` in the capability map.
Each entry carries:

- `idx` (uint) — the `uart_idx` used by SET_PHY / GET_PHY.
- `cdc` (uint) — CDC-ACM index carrying the byte stream.
- `backend` (text) — `"hw"` or `"pio"`.
- `max_baud` (uint).
- `phys` (text-array) — the list of phys this instance supports
  (`["ttl"]` for PIO, `["ttl", "rs232", "rs485"]` for HW).
- `de_pin` (uint, optional) — present only when the instance is
  RS485-capable.

Feature flags `"uart.ttl"`, `"uart.rs232"`, `"uart.rs485"` are
advertised in the top-level `features` array so hosts can gate
capability-based behaviour cheaply without walking the bus array.
