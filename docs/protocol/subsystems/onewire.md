# RPBP — 1-Wire subsystem (subsys = `0x09`)

Maxim/Dallas 1-Wire bus master implemented on a single PIO state
machine on PIO1. Standard speed only (~70 µs per bit slot, ~1 ms
reset). Overdrive speed is a future extension.

The PIO program emits one bit-time slot per FIFO command and pushes
the sampled bus level back; the firmware composes 8-bit bytes
(LSB-first per Maxim AN126) on top. Three RPBP opcodes — RESET,
WRITE, READ — cover normal slave traffic; TRIPLET is the SEARCH-ROM
primitive that lets a host enumerate slaves with one round-trip per
ROM bit.

Hardware: a single GPIO with an external 4.7 kΩ pull-up to +3V3 (a
weaker on-chip pull-up is enabled as a backup but is too soft for
fast slaves). The PIO program emulates open-drain by toggling
pindirs.

Access path: vendor RPBP. Linux exposes the bus via `rpbridge.ko`
once an upstream `kernel::w1::Master` Rust wrapper exists; the
mainline `w1_master_driver` shape is small enough that the
adaptation is straightforward — see driver/TODO.md #M12.

## Capabilities

`buses.onewire[*]` carries:

| key        | type   | meaning                              |
|------------|--------|--------------------------------------|
| `idx`      | uint   | bus index (always `0` in v1)         |
| `data_pin` | uint   | GPIO carrying the data line          |
| `speed`    | text   | `"standard"` (only option in v1)     |

Feature flag advertised when the build includes 1-Wire support:
`onewire.master`.

## Opcodes

### `0x00` GET_INFO

Request: empty.

Response (4 bytes): `[count u8][pin u8][speed u8][reserved u8]`.

* `count` — `1` if a bus is online, `0` if init failed.
* `pin` — GPIO number.
* `speed` — `0` = standard.

### `0x01` RESET

Request: `[idx u8]`.

Response (1 byte): `[presence u8]`. `0` = no slaves, `1` = at least
one slave asserted a presence pulse.

### `0x02` WRITE

Request: `[idx u8][bytes …]`. Bytes are sent LSB-first per slot.

* Total wire payload ≤ MTU (4 KiB) → max ~4 KiB per WRITE; chunk
  larger transfers across multiple WRITEs.
* No reply payload — status only.

### `0x03` READ

Request (3 bytes): `[idx u8][len LE u16]`. `len` ∈ `[1, 256]`.

Response: the read bytes, LSB-first.

### `0x04` TRIPLET

Request (2 bytes): `[idx u8][dir u8]`. `dir` is the search direction
to write (0 or 1).

Response (1 byte): packed result —

| Bit | Meaning                                    |
|-----|--------------------------------------------|
| 0   | `id_bit` — the read true bit               |
| 1   | `cmp_bit` — the read complement bit        |
| 2   | `dir`    — the bit that was actually written |

Used by the SEARCH-ROM state machine (Maxim AN187): host issues
TRIPLET 64 times per slave, picking `dir` based on the
(id_bit, cmp_bit) pair to walk the binary tree of ROM IDs.

## Examples

### Enumerate the bus, read a DS18B20

```
RESET                                          → presence = 1
WRITE  bytes=[0xCC, 0x44]                      # SKIP_ROM, CONVERT_T
                                               # (wait ≥ 750 ms for conversion)
RESET                                          → presence = 1
WRITE  bytes=[0xCC, 0xBE]                      # SKIP_ROM, READ_SCRATCHPAD
READ   len=9                                   → 9 scratchpad bytes
                                               # bytes 0..1 = temperature LE i16
```

### SEARCH-ROM (find a single slave)

```
RESET                                          → presence = 1
WRITE  bytes=[0xF0]                            # SEARCH_ROM
loop 64 times:
  TRIPLET dir=<from AN187 state machine>       → id_bit, cmp_bit, dir
                                               # accumulate dir into ROM ID
```

## Error semantics

| Status     | Cause                                                  |
|------------|--------------------------------------------------------|
| `ENOENT`   | `idx` ≠ 0                                              |
| `EINVAL`   | bad len (0 or > 256), short payload                    |
| `EAGAIN`   | bus not initialised (subsystem disabled at build)      |
| `EBUSY`    | pin/PIO conflict at firmware boot                      |
| `ENOTSUP`  | board built without `RPBRIDGE_HAVE_ONEWIRE`            |

## Performance notes

A 9-byte READ (e.g., DS18B20 scratchpad) takes 9 × 8 × 70 µs = ~5 ms
of bus time. A full SEARCH ROM with one slave on the bus takes
1 + 64 = 65 round-trips of ~70 µs each = ~5 ms bus time, plus the
USB latency per opcode (~1 ms USB FS round-trip). For continuous
temperature polling at ≥ 1 Hz this is comfortably within budget;
for fast SEARCH on a bus with many slaves, batch slot count
optimisation lives on the host side.
