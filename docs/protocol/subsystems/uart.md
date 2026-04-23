# RPBP v1 — UART subsystem

**Subsystem id: `0x06`.** Primarily a *diagnostic and override* path —
normal operation of the UARTs uses the standard CDC-ACM class and
does not need RPBP at all.

The RPBridge firmware advertises **four virtual COM ports** on
every host:

| CDC | Interface # | Phy    | Backend  | Connector         |
|----:|------------:|--------|----------|-------------------|
| 0   | 0           | RS232  | UART0 HW | DE-9 male         |
| 1   | 2           | RS485  | UART0 HW | Phoenix terminal  |
| 2   | 4           | TTL    | UART0 HW | 4-pin 2.54 header |
| 3   | 6           | TTL    | PIO-UART | 4-pin 2.54 header |

Opening any of these CDCs in a stock terminal (`minicom`, `picocom`,
PuTTY, Tera Term, `System.IO.Ports.SerialPort`, …) is enough to
communicate over that physical layer. **No driver, no helper, no
RPBP command is required.** The firmware auto-selects the UART0
transceiver from whichever of CDC 0..2 currently has DTR asserted.

The RPBP UART subsystem below is only for the few cases the
driverless path cannot cover: (a) terminals that do not assert DTR,
(b) scripted setups that want explicit phy control before opening a
CDC, (c) diagnostic read-back of the current phy.

## Host-facing semantics

### Driverless phy auto-switch (default)

```text
User opens /dev/rpbridge-uart1-rs232 (or COMn labelled "RS232")
          │
          │ CDC SET_CONTROL_LINE_STATE (DTR=1)
          ▼
    firmware: uart_phy_set(RS232) + mark CDC 0 as active
          │
          │ all bytes flow CDC 0 ↔ UART0 HW
          ▼
User closes port  →  DTR=0  →  firmware drops phy to OFF
```

If a second user then opens `/dev/rpbridge-uart1-rs485`, the
firmware:

1. Observes DTR=1 on CDC #1.
2. Calls `uart_phy_set(RS485)` — MAX3232 shuts down, SN65HVD72
   activates, DE pin handed to `uart_hw`.
3. Marks CDC #1 as the new active owner of UART0 HW.
4. Silently drains any bytes still sitting in the RX FIFO of the
   previously-active CDC (CDC #0) so the host side does not stall.

Break signals (`tcsendbreak`, `SendBreak`) are forwarded to UART0
only on the currently-active CDC. Line coding changes
(`stty`, `SetLineCoding`) are applied to UART0 regardless of which
CDC sent them — a fresh `stty 9600` on any of the three re-programs
the live link.

CDC #3 (PIO-UART) is independent: DTR is informational; bytes flow
regardless of CDC 0..2 state.

### Explicit override (RPBP SET_PHY)

Useful when the host terminal does not manipulate DTR, or when a
CI script wants to park the hardware in a specific state before any
port is opened.

Opcode summary:

| Opcode | Name       | Purpose                                           |
|-------:|------------|----------------------------------------------------|
| `0x00` | `SET_PHY`  | Force a phy mode regardless of DTR state.         |
| `0x01` | `GET_PHY`  | Read back the current phy mode (diagnostic).      |

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

- `idx` (u8) — UART instance. `0` = UART0 HW (phy-bank-capable).
  `1` = PIO-UART (TTL-only — rejects non-TTL phys with `ENOTSUP`).
- `phy` (u8) — desired phy mode:
  - `0` = **OFF** (transceivers tri-stated, line idle)
  - `1` = **TTL** (direct 3.3 V drive to TTL header #1)
  - `2` = **RS232** (MAX3232 active, SN65HVD72 shut down)
  - `3` = **RS485** (SN65HVD72 active, MAX3232 shut down, DE
    pin toggled automatically around every TX burst)

**Response (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x06  |  0x00  | status |
 +--------+--------+--------+
```

- `status = OK (0)` on success.
- `status = ENOTSUP (10)` for a non-TTL phy on a TTL-only instance.
- `status = ENOENT (4)` for an unknown `idx`.
- `status = EINVAL (2)` for a `phy` value outside `0..3`.
- `status = EMSGSIZE (7)` if the request payload length isn't 4.

**Interaction with DTR:**

- SET_PHY takes effect **immediately**, regardless of any current
  DTR state on the UART0 CDCs.
- If a subsequent CDC DTR transition (either assert or deassert)
  arrives, the firmware re-derives the phy from that CDC and the
  override is effectively replaced. That is, SET_PHY is a one-shot
  override, not a lock — the DTR-driven state machine keeps running.
- Hosts that want a stable phy with the driverless flow should open
  the correct CDC instead. RPBP SET_PHY is for the edge cases, not
  day-to-day use.

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

- `phy` (u8) matches the encoding above. TTL-only instances
  always return `phy = 1` (TTL).

`status = ENOENT` for an unknown `idx`; `EMSGSIZE` for the wrong
request length.

## RS485 DE timing

When `phy = RS485` is active (by DTR or by SET_PHY), the firmware
drives the DE pin (GP6 on rev-B) HIGH just before the first bit of
every TX burst enters the UART shift register, and releases it back
to LOW once the hardware BUSY flag clears — i.e. after the last
stop bit has fully shifted out.

The scaffold uses a software state machine ticked from the main
loop (see `firmware/src/peripherals/uart_hw.c::uart_hw_tick`). A later
PIO applet may replace it for bit-precise timing at > 1 Mbaud, but
the current implementation is timing-correct at all supported baud
rates (up to 3 Mbaud) with a tick period ≤ 5 ms.

## Line coding

Line coding (baud, data bits, stop bits, parity) is set via the
standard CDC-ACM `SetLineCoding` class request on the respective
CDC interface — **not** through this subsystem. The firmware
forwards `SetLineCoding` directly to the underlying UART (HW or
PIO). Any of the three UART0 CDCs may reconfigure UART0 since they
share the same hardware backend.

### HW UART (CDC #0..2)

Honours the full CDC-ACM line-coding matrix: 5..8 data bits, 1 or 2
stop bits, parity NONE / ODD / EVEN, baud 0..3 Mbaud. Mark / space
parity is not supported and is treated as NONE.

### PIO UART (CDC #3) — 8N1 only

The PIO-UART is driven by hand-written `.pio` programs that are
hard-wired to **8 data bits, no parity, 1 stop bit (8N1)**. The
firmware cannot honour other framings without shipping additional
PIO programs and a runtime selector, so it handles non-8N1
`SetLineCoding` requests as follows:

- The baud rate is always applied verbatim (the SM clock divider is
  framing-independent).
- Data bits / stop bits / parity that differ from 8 / 1 / NONE are
  clamped to 8N1 on the wire.
- A per-instance counter (`framing_clamped`) is incremented so hosts
  can detect the divergence via SELFTEST / stats read-out.
- The first clamped request per boot also emits a one-shot warning
  on the debug UART (UART1 on GP4/GP5) for bring-up diagnostics.

The constraint is advertised up-front in two places in the
capability map:

1. Feature flag `"uart.pio.8n1-only"` in the top-level `features`
   array — cheap to test.
2. Explicit `framing: ["8n1"]` key on the PIO-UART bus entry under
   `buses.uart` — machine-readable and future-proof. HW UART
   entries omit this field, which a host MAY interpret as "any
   standard CDC-ACM framing supported".

Host userspace SHOULD probe either channel before opening the
PIO COM port with a non-8N1 `termios` config and warn the user
rather than silently ship corrupted bytes.

## Capability-map advertising

Each CDC appears under `buses.uart` in the capability map. Each
entry carries:

- `idx` (uint) — the logical UART instance (0 = UART0 HW, 1 = PIO).
- `cdc` (uint) — the CDC-ACM index carrying the byte stream.
- `backend` (text) — `"hw"` or `"pio"`.
- `phy` (text) — `"rs232" | "rs485" | "ttl"`.
- `max_baud` (uint).
- `de_pin` (uint, optional) — present only on RS485 entries.
- `framing` (text array, optional) — list of supported line-coding
  framings. Present on backends with framing restrictions (e.g. the
  PIO UART, which lists `["8n1"]`). Absent = any standard CDC-ACM
  framing is accepted by the backend.

Feature flags `"uart.ttl"`, `"uart.rs232"`, `"uart.rs485"`, and
`"uart.pio.8n1-only"` are advertised in the top-level `features`
array so hosts can gate capability-based behaviour cheaply without
walking the bus array.
