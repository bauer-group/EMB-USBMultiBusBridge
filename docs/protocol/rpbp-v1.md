# RPBP v1 — protocol specification

**Status:** Draft. This document is the authoritative reference for
the RPBridge Protocol version 1 (RPBP v1). If it disagrees with any
design draft under `docs/drafts/`, **this document wins** — drafts
are preserved for historical context only.

## 1. Transport

RPBP rides on a dedicated USB vendor interface with one bulk IN and
one bulk OUT endpoint, wMaxPacketSize 64 (USB 2.0 Full-Speed). The
interface is bound to `winusb.sys` on Windows via MS OS 2.0
descriptors; on Linux it is claimed by the RPBridge kernel driver.

CAN uses a **separate** `gs_usb`-compatible vendor interface; UART
traffic flows over three standard CDC-ACM pairs. Neither is covered
by this protocol — they are explicitly out of scope.

## 2. Frame layout

```text
+----------------+-----------------------+-----------------+
|  16 B header   |  0..MAX_PAYLOAD_LEN   |  4 B CRC-32C    |
+----------------+-----------------------+-----------------+
```

`MAX_PAYLOAD_LEN` is 4096 bytes in v1. Larger messages MUST be split
using the `FRAGMENT` / `LAST` flags; every fragment carries its own
header and CRC.

## 3. Header

Sixteen bytes, little-endian:

| Offset | Size | Field          | Notes                                         |
|-------:|-----:|----------------|-----------------------------------------------|
|  0     |  1   | `magic`        | `0x52` (`'R'`).                               |
|  1     |  1   | `version`      | `0x01` for v1.                                |
|  2     |  1   | `msg_type`     | See [§4](#4-message-types).                  |
|  3     |  1   | `flags`        | See [§5](#5-flags).                           |
|  4     |  2   | `channel`      | See [§6](#6-channels).                        |
|  6     |  2   | `seq`          | Monotonic per channel, wraps at 65 536.       |
|  8     |  4   | `payload_len`  | Bytes of payload (excludes header + CRC).     |
| 12     |  4   | `timestamp_us` | Device-boot microseconds.                     |

## 4. Message types

| Value  | Name            | Direction           | Purpose                               |
|-------:|-----------------|---------------------|---------------------------------------|
| `0x00` | `HELLO`         | both                | Session init; version + identity.     |
| `0x01` | `CAPABILITIES`  | device → host       | CBOR capability map.                  |
| `0x02` | `CMD_REQUEST`   | host → device       | Synchronous command.                  |
| `0x03` | `CMD_RESPONSE`  | device → host       | Response to `CMD_REQUEST`.            |
| `0x04` | `STREAM_DATA`   | either              | Payload frame on a stream channel.    |
| `0x05` | `STREAM_CREDIT` | host → device       | Flow-control credit grant.            |
| `0x06` | `EVENT`         | device → host       | Unsolicited event.                    |
| `0x07` | `PING`          | host → device       | Round-trip measurement / keep-alive.  |
| `0x08` | `PONG`          | device → host       | Response to `PING`.                   |
| `0x09` | `ERROR`         | device → host       | Error tied to a previous `seq`.       |
| `0x0A` | `RESET_CHANNEL` | host → device       | Flush and re-initialise a channel.    |
| `0x0B` | `TIME_SYNC`     | both                | PTP-lite timestamp negotiation.       |

Values `0x10`..`0x7F` are reserved for minor-version additions.
Values `0x80`..`0xFF` are reserved for vendor experimentation and
MUST NOT appear in public releases.

## 5. Flags

| Bit | Name           | Meaning                                         |
|-----|----------------|-------------------------------------------------|
| 0   | `CBOR`         | Payload is CBOR-encoded.                        |
| 1   | `COMPRESSED`   | Payload is LZ4-block-compressed (opt-in).       |
| 2   | `URGENT`       | Out-of-band precedence (control channel only).  |
| 3   | `FRAGMENT`     | Fragment; not the last one of its sequence.    |
| 4   | `LAST`         | Last fragment of a multi-frame message.         |
| 5   | `CONTINUATION` | Middle fragment (neither first nor last).       |
| 6,7 | reserved       | **Must be zero** in v1. Receivers reject frames where either bit is set. |

## 6. Channels

| Range          | Purpose                                          |
|----------------|--------------------------------------------------|
| `0`            | Control — `HELLO`, `CAPABILITIES`, `CMD_*`, `TIME_SYNC`, `RESET_CHANNEL`. |
| `1`            | Events — asynchronous device-to-host notifications. |
| `2`..`15`      | Reserved for future static assignments.          |
| `16`..`239`    | Dynamic streams — assigned via `CMD_REQUEST{ OPEN_CHANNEL }` and bound to logical functions (`uart0`, `adc_stream0`, `logic_capture`, …). |
| `240`..`254`   | Vendor experimentation (never in public firmware). |
| `255` (`0xFF`) | Broadcast — rare, used for global device-side notices. |

The control (`0`) and events (`1`) channels are **fixed for the life
of the protocol** and MUST never be reassigned.

## 7. CRC-32C

Trailing 4-byte CRC-32C (Castagnoli polynomial `0x1EDC6F41`, reflected,
init `0xFFFF_FFFF`, final XOR `0xFFFF_FFFF`) computed over
`header || payload`. Matches the kernel's `libcrc32c`, the firmware's
`rpbp_crc32c()`, and x86_64 `crc32` / ARMv8 `CRC32CX` hardware
implementations bit-for-bit.

RFC 3720 test vector: `CRC32C("123456789") == 0xE3069283`.

## 8. Flow control

Stream channels use a credit-grant scheme. The host publishes
`STREAM_CREDIT` messages carrying `(channel, credits)`; the device
may send at most `credits` bytes on that channel before blocking.
Initial per-channel credit is advertised in the capability map
(`max_rx_inflight[channel]`).

Unbounded per-channel queues are explicitly rejected: a saturated
logic-capture stream must not be able to starve CAN RX.

## 9. Capability discovery

Immediately after attach, the host issues
`CMD_REQUEST{ GET_CAPABILITIES }` with the `CBOR` flag set. The
device answers with a CBOR map — see
[`capabilities.md`](capabilities.md) for the full schema.

Unknown keys in the response MUST be ignored (forward compatibility).
Hosts MUST NOT assume a field is present — read-optional, write-explicit.

## 10. Versioning

| Bump     | Meaning                                             | Compatibility                |
|----------|-----------------------------------------------------|------------------------------|
| MAJOR    | Header layout / message-type semantics change.      | Incompatible; refuse to run. |
| MINOR    | New message types, new flags (outside reserved), new channels, new capability keys. | Forward-compatible. |
| PATCH    | Specification clarifications, new test vectors.    | Fully compatible.            |

The `proto` tuple in the capability map reports
`(MAJOR, MINOR, PATCH)`. Hosts with `MINOR_host < MINOR_device` MUST
ignore the subsystems they don't understand.

## 11. Status codes

See [`status-codes.md`](status-codes.md).

## 12. Reference implementations

- Canonical Rust: `userspace/rpbridge-protocol/`.
- Firmware C: `firmware/src/rpbp/`.
- Kernel Rust: `driver/src/rpbp.rs`.

All three are exercised by the shared golden-vector corpus under
[`tests/golden/`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/tree/main/tests/golden).
