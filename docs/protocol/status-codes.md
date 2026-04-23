# RPBP v1 — status codes

`CMD_RESPONSE` and `ERROR` payloads lead with a single 8-bit
status byte. Values are POSIX-errno-inspired and **stable** — they
MUST NOT be renumbered between releases.

| Code   | Name          | Description                                                 |
|-------:|---------------|-------------------------------------------------------------|
| `0x00` | `OK`          | Success.                                                    |
| `0x01` | `EAGAIN`      | Transient; retry after the advised back-off.                |
| `0x02` | `EINVAL`      | Invalid parameters.                                         |
| `0x03` | `EPERM`       | Operation forbidden (channel not open, bus reserved, …).    |
| `0x04` | `ENOENT`      | Unknown channel or feature.                                 |
| `0x05` | `EIO`         | Low-level I/O error (I²C NACK, CAN bus-off, SPI underrun…). |
| `0x06` | `ETIMEDOUT`   | Command timed out on the device.                            |
| `0x07` | `EMSGSIZE`    | Payload too large (per `MAX_PAYLOAD_LEN` or capability MTU).|
| `0x08` | `ECRC`        | CRC failure in received data (reported, not this frame's).  |
| `0x09` | `EPROTO`      | Protocol violation (should never happen; indicates a bug).  |
| `0x0A` | `ENOTSUP`     | Feature not implemented — check the capability map first.   |
| `0x0B` | `EBUSY`       | Channel or bus busy.                                        |
| `0x0C` | `ECANCELED`   | Host-cancelled operation.                                   |
| `0x0D` | `ENOSPC`      | Device-side FIFO full.                                      |
| `0x0E` | `EOVERFLOW`   | Credit underflow — indicates a protocol bug, log and assert.|
| `0x10`–`0x1F` | reserved | I²C-specific extensions (arbitration loss, …).         |
| `0x20`–`0x2F` | reserved | SPI-specific extensions.                               |
| `0x40`–`0x4F` | reserved | UART-specific extensions (break, framing, parity).     |
| `0xF0`–`0xFF` | vendor   | Local experimentation; never in public firmware.        |

Consumers MUST treat unknown codes as "generic failure" and log them
with their raw numeric value — do not silently remap to the nearest
known code.
