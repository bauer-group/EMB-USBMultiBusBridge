# RPBP v1 — protocol specification

**Status:** Stable (v1.0.0). Authoritative for the RPBridge Protocol
version 1. Disagreements with design drafts under `docs/drafts/` are
resolved in favour of this document.

This specification is normative: firmware, kernel driver and userspace
implementations MUST conform, and the shared golden-vector corpus under
[`tests/golden/`](https://github.com/bauer-group/EMB-USBMultiBusBridge/tree/main/tests/golden)
MUST validate every commit that touches wire behaviour.

---

## 1. Transport

RPBP rides on a single USB vendor interface. Two bulk endpoints,
`wMaxPacketSize = 64` (USB 2.0 Full-Speed). Linux binds it through the
RPBridge kernel driver; Windows auto-binds `winusb.sys` via
MS OS 2.0 descriptors.

CAN is carried **separately** on a dedicated `gs_usb`-compatible
vendor interface (mainline `drivers/net/can/usb/gs_usb.ko` binds it).
UART traffic uses three standard CDC-ACM pairs. Neither is covered
by this document.

**ZLPs and USB packet boundaries are irrelevant at the RPBP layer.**
Receivers determine frame length from the header's `payload_len`
field, not from USB transfer boundaries. A sender MAY append a
zero-length packet for low-latency flush; a receiver MUST ignore it.

## 2. Frame layout

```text
+----------------+-----------------------+-----------------+
|  16 B header   |  0..MAX_PAYLOAD_LEN   |  4 B CRC-32C    |
+----------------+-----------------------+-----------------+
```

Total maximum frame length: `HEADER_LEN + MAX_PAYLOAD_LEN + CRC_LEN =
4116 bytes`.

`MAX_PAYLOAD_LEN = 4096` in v1. Messages larger than this MUST be split
using the `FRAGMENT`/`LAST` flags; see [#7](#7-fragmentation).

## 3. Header

Sixteen bytes, **little-endian** for all multi-byte integers:

| Offset | Size | Field          | Notes                                         |
|-------:|-----:|----------------|-----------------------------------------------|
|  0     |  1   | `magic`        | `0x52` (`'R'`). Receivers reject non-matching frames with `EPROTO`. |
|  1     |  1   | `version`      | `0x01` for v1. Non-matching versions return `EPROTO`; the device MAY reply with a `HELLO` to renegotiate. |
|  2     |  1   | `msg_type`     | See [#4](#4-message-types).                   |
|  3     |  1   | `flags`        | See [#5](#5-flags).                           |
|  4     |  2   | `channel`      | See [#6](#6-channels).                        |
|  6     |  2   | `seq`          | Monotonic per channel. Wraps at 65 536.       |
|  8     |  4   | `payload_len`  | Net payload bytes (excludes header + CRC). Max 4096. |
| 12     |  4   | `timestamp_us` | Sender's µs-since-boot at the moment of serialisation. 32-bit wraps at ~71.6 minutes. |

The header is fixed-layout; receivers MUST NOT assume C-struct
packing matches this layout and MUST decode field-by-field.

## 4. Message types

`msg_type` is the RPBP-level envelope. Subsystem-level operation
codes live inside `CMD_REQUEST`/`CMD_RESPONSE` payloads, not here.

| Value  | Name            | Direction       | Purpose                                |
|-------:|-----------------|-----------------|----------------------------------------|
| `0x00` | `HELLO`         | both            | Session init. Identity + version + nonce. |
| `0x01` | `CAPABILITIES`  | device → host   | CBOR capability map (cf. `capabilities.md`). |
| `0x02` | `CMD_REQUEST`   | host → device   | Synchronous command. Payload carries subsystem + opcode. |
| `0x03` | `CMD_RESPONSE`  | device → host   | Response to `CMD_REQUEST`. Mirrors `(channel, seq)` of the request. |
| `0x04` | `STREAM_DATA`   | either          | Payload on a stream channel (dynamic channels 16..239). |
| `0x05` | `STREAM_CREDIT` | host → device   | Grant of `N` bytes of credit on a stream channel. |
| `0x06` | `EVENT`         | device → host   | Unsolicited event on channel 1 (events). |
| `0x07` | `PING`          | host → device   | Round-trip probe / keep-alive / time sync. |
| `0x08` | `PONG`          | device → host   | Response to `PING`. Mirrors `seq`. |
| `0x09` | `ERROR`         | device → host   | Error tied to a previous `(channel, seq)`. |
| `0x0A` | `RESET_CHANNEL` | host → device   | Flush channel state, reset `seq`, release credits. |
| `0x0B` | `TIME_SYNC`     | both            | Explicit time-sync exchange (alternative to PING). |
| `0x10`..`0x7F` | reserved |                | Reserved for additive minor-version extensions. |
| `0x80`..`0xFF` | vendor  |                 | Vendor experimentation. MUST NOT appear in public releases. |

## 5. Flags

| Bit | Name           | Meaning                                                   |
|-----|----------------|-----------------------------------------------------------|
| 0   | `CBOR`         | Payload is CBOR-encoded (RFC 8949).                       |
| 1   | `COMPRESSED`   | Payload is LZ4-block-compressed. Opt-in; advertised via capability feature `"compression.lz4"`. |
| 2   | `URGENT`       | Out-of-band precedence. Control channel only.             |
| 3   | `FRAGMENT`     | Frame is a fragment and **not** the last of its sequence. |
| 4   | `LAST`         | Frame is the **last** fragment of a multi-frame message. On a single-frame message: MUST NOT be set alongside `FRAGMENT`. |
| 5   | `CONTINUATION` | Middle fragment — neither first nor last. Equivalent to `FRAGMENT=1, LAST=0` on a non-first fragment. Provided for readability; receivers MUST accept either encoding. |
| 6,7 | reserved       | **Must be zero** in v1. Receivers reject frames where either bit is set with `EPROTO`. |

`FRAGMENT` and `LAST` are mutually exclusive on the same frame
**only** in their strict sense: a single-fragment message has neither
set; a first fragment has `FRAGMENT=1, LAST=0`; intermediate fragments
have `FRAGMENT=1, LAST=0` (optionally also `CONTINUATION=1`); the
final fragment has `FRAGMENT=0, LAST=1`.

## 6. Channels

| Range          | Purpose                                                          |
|----------------|------------------------------------------------------------------|
| `0`            | **Control** — HELLO, CAPABILITIES, CMD_REQUEST / CMD_RESPONSE, TIME_SYNC, RESET_CHANNEL, PING, PONG, ERROR. |
| `1`            | **Events** — asynchronous device-to-host notifications (EVENT msg_type only). |
| `2`..`15`      | Reserved for future static assignments.                          |
| `16`..`239`    | **Dynamic streams** — assigned by `CMD_REQUEST{OPEN_CHANNEL}`; bound to logical functions (`uart0`, `adc_stream0`, `logic_capture`, …). |
| `240`..`254`   | Vendor experimentation. Never in public firmware.                |
| `255` (`0xFF`) | **Broadcast** — reserved for global device-side notices.          |

Channel `0` and `1` are **fixed for the life of the protocol**. MAJOR
version bumps MAY reassign channels `2..239`; MINOR bumps MAY only add.

## 7. Fragmentation

Frames larger than `MAX_PAYLOAD_LEN` MUST be split before
transmission. Fragmentation is per-channel and has strict ordering
rules:

- Fragments of a single message MUST be sent with **strictly
  monotonic, gap-free** `seq` numbers. Receivers MUST reject any frame
  where `seq != expected_next_seq` while a fragment re-assembly is
  in progress on that channel (`EPROTO`).
- **No interleaving.** A channel MUST complete one multi-fragment
  message before starting another. Senders MUST either send a
  complete message in one or more fragments before starting the next,
  or use separate channels for concurrent streams.
- The first fragment has `FRAGMENT=1, LAST=0`.
- Intermediate fragments have `FRAGMENT=1, LAST=0` (optionally
  `CONTINUATION=1`).
- The final fragment has `FRAGMENT=0, LAST=1`.
- Every fragment carries the full RPBP header and a full per-frame
  CRC-32C. The CRC is computed per fragment, not over the reassembled
  message.
- The reassembled payload is the byte concatenation of the fragments'
  payloads, in seq order. Fragment count is not carried; the receiver
  stops reassembling on `LAST=1`.
- Each fragment's `payload_len` is independent and MAY be up to
  `MAX_PAYLOAD_LEN`.
- Maximum reassembled message size is implementation-defined but MUST
  be at least 64 kB for v1 conformance. The capability map advertises
  a higher ceiling where supported.

If a channel is reset (`RESET_CHANNEL`) or disconnected mid-stream,
partial reassembly state is discarded and a new sequence begins at
seq `0`.

## 8. CRC-32C

Trailing 4-byte CRC using the Castagnoli polynomial
`0x1EDC6F41`, reflected, `init = 0xFFFFFFFF`, final XOR
`0xFFFFFFFF`. Computed over `header || payload`. Bit-identical to:

- `libcrc32c` / `<linux/crc32c.h>` on Linux.
- x86_64 SSE4.2 `crc32` and ARMv8 `CRC32CX` hardware paths.
- `rpbp_crc32c()` in the firmware (`firmware/src/rpbp/rpbp_crc32c.c`).
- `rpbridge_protocol::crc32c()` in the userspace Rust crate.

RFC 3720 reference vector: `CRC32C("123456789") == 0xE3069283`.

Receivers MUST verify the CRC before consuming any payload field.
CRC failure is reported to the user as `ECRC`; for a device-side
failure, the sender is sent an `ERROR` frame (see [#12](#12-error-frames)).

## 9. Session initialisation (HELLO flow)

Immediately after interface enumeration, the host MUST initiate the
session with a `HELLO` on channel 0, `seq = 0`:

```text
Host -> Device   msg_type = HELLO, channel = 0, seq = 0, flags = CBOR
                 payload (CBOR map):
                   {
                     "proto": [1, 0, 0],
                     "host":  { "os": "linux", "impl": "rpbridge-rs/0.1.0" },
                     "nonce": h'<16 random bytes>'
                   }
```

The device MUST reply with a `HELLO` on channel 0, same `seq = 0`:

```text
Device -> Host   msg_type = HELLO, channel = 0, seq = 0, flags = CBOR
                 payload (CBOR map):
                   {
                     "proto":  [1, 0, 0],
                     "fw":     "0.1.0+abc123",
                     "board":  "rpbridge-rev-B",
                     "serial": h'<8 bytes, flash unique ID>',
                     "nonce":  h'<echoed host nonce>',
                     "features": ["cbor", "credit-fc", "time-sync"]
                   }
```

If the major protocol versions differ, the device MUST respond with
`ERROR` containing `status = ENOTSUP` and close the connection
semantically (ignore subsequent frames until the next HELLO).

After a successful HELLO exchange, the host MAY immediately issue
`CMD_REQUEST{GET_CAPABILITIES}` (see [`capabilities.md`](capabilities.md)
and [`subsystems/sys.md`](subsystems/sys.md)).

## 10. Command structure (CMD_REQUEST / CMD_RESPONSE)

Every `CMD_REQUEST` and `CMD_RESPONSE` carries a **compact binary
header** followed by per-opcode arguments:

```text
 0        1        2                                           N
 +--------+--------+-------------------------------------------+
 | subsys | opcode |                 args                      |
 +--------+--------+-------------------------------------------+
```

- `subsys` (u8) identifies the subsystem (SYS=0, I2C=1, SPI=2,
  GPIO=3, PWM=4, ADC=5, UART=6, reserved 7–127, vendor 128–255).
- `opcode` (u8) identifies the operation within the subsystem.

### 10.1 Response format

```text
 0        1        2        3                                   N
 +--------+--------+--------+-----------------------------------+
 | subsys | opcode | status |            result_args            |
 +--------+--------+--------+-----------------------------------+
```

- `subsys`, `opcode` mirror the request.
- `status` (u8) is a status code from [`status-codes.md`](status-codes.md).
  `status == 0` (`OK`) indicates success; non-zero indicates failure,
  and the `result_args` MAY be empty or MAY contain extended diagnostic
  fields (opcode-specific).

### 10.2 CBOR-encoded commands

If the request carries the `CBOR` flag, the payload is instead:

```cbor
{
  "s": <subsys-uint>,
  "o": <opcode-uint>,
  "a": <args-map-or-array>
}
```

The response (also `CBOR`-flagged) is:

```cbor
{
  "s": <subsys>,
  "o": <opcode>,
  "st": <status-uint>,
  "r": <result-map-or-array-or-omitted>
}
```

Binary is the default for hot-path operations (I²C/SPI/GPIO); CBOR is
preferred for capability discovery and operations with optional/
forward-compatible fields.

## 11. Channel flow control

Stream channels (16..239) use **credit-based flow control**:

1. On channel open (via `CMD_REQUEST{OPEN_CHANNEL}`), the capability
   map's `max_rx_inflight[channel]` advertises the initial credit
   budget in bytes.
2. Each `STREAM_DATA` frame the device sends consumes
   `payload_len` bytes from the channel's remaining credit.
3. When credit reaches zero, the device MUST stop transmitting on
   that channel and SHOULD emit an `EVENT{FLOW_BLOCKED, channel=N}`.
4. The host issues `STREAM_CREDIT(channel=N, credits=B)` to grant
   `B` additional bytes. The payload is a single little-endian u32.
5. Credits are cumulative; there is no upper bound beyond the host's
   local buffer capacity.

Flow control applies per channel. Control channel (0) and events
channel (1) do **not** use credits — they are sized to small
message counts and any overrun is a protocol bug on the device side.

## 12. Error frames

When the device encounters a frame it cannot process successfully, it
responds with an `ERROR` frame on the control channel:

```text
 0        1        2        3                                 N
 +--------+--------+--------+---------------------------------+
 | status | orig_channel (LE u16) | orig_seq (LE u16)         |
 +--------+--------+--------+---------------------------------+
 | reason_len (LE u16)     | reason (UTF-8, optional)         |
 +-------------------------+----------------------------------+
```

- `status` — one of the codes in [`status-codes.md`](status-codes.md).
- `orig_channel`, `orig_seq` — the `(channel, seq)` of the frame that
  triggered the error. Allows the host to tie the error back to its
  original request without relying on response ordering.
- `reason_len` — byte length of the optional `reason` UTF-8 string.
  Zero means no human-readable reason is provided.
- `reason` — optional short UTF-8 string (≤ 255 bytes) to aid
  debugging. Hosts MUST NOT rely on its content for control flow.

Devices MUST generate `ERROR` frames for at least:

- CRC failures on control-channel requests (`status = ECRC`).
- Malformed/unknown `msg_type` or reserved-bit-set frames
  (`status = EPROTO`).
- Unknown subsystem or opcode in `CMD_REQUEST` (`status = ENOENT`).
- Payload size violations (`status = EMSGSIZE`).
- Fragmentation ordering violations (`status = EPROTO`).

Errors on event-channel frames are silently dropped — events are
best-effort.

## 13. Time synchronisation

Two mechanisms:

### 13.1 Implicit — every frame

Every frame carries `timestamp_us` in its header. Hosts synchronising
a device clock to `CLOCK_MONOTONIC_RAW` MAY track the
`(device_ts, host_ts_rx)` pairs on arriving frames and run a
simple linear regression. Accuracy: ±200 µs under idle load.

### 13.2 Explicit — PING/PONG or TIME_SYNC

For higher precision:

- Host sends `PING` on channel 0 at host `t1 = host_ts_send_us`.
- Device captures `t2 = device_ts_rx_us` on receipt.
- Device sends `PONG` with `timestamp_us = t3 = device_ts_send_us`
  (standard header field) and carries in its payload the single
  little-endian u32 `t2`.
- Host captures `t4 = host_ts_rx_us` on arrival.

Offset and one-way delay (per NTP/PTP):

```text
offset = ((t2 - t1) + (t3 - t4)) / 2
delay  = ((t4 - t1) - (t3 - t2)) / 2
```

`TIME_SYNC` (msg_type `0x0B`) is an alias for this exchange when the
host prefers an explicit message type. Payload and semantics are
identical to PING/PONG.

### 13.3 Wraparound

`timestamp_us` is a 32-bit microsecond counter; it wraps roughly every
71.6 minutes. Hosts SHOULD re-sync at least every 30 minutes to stay
within one wraparound of the device clock.

## 14. Session teardown and reconnection

There is no explicit teardown message. Either side disconnects by
detaching the USB interface. On reconnection, the host MUST start a
new HELLO exchange; state on both sides is discarded.

`RESET_CHANNEL(channel=N)` resets a single channel without tearing
down the session: device-side pending work for `N` is cancelled, seq
counters for `N` reset to `0`, any reassembly in progress on `N` is
discarded, and credits on `N` revert to the advertised
`max_rx_inflight[N]`. The host MUST NOT reuse sequence numbers or
rely on in-flight state after issuing RESET_CHANNEL.

## 15. Ordering and duplicates

USB bulk transfers are in-order per endpoint pair; implementations
MAY assume the transport does not reorder frames. Duplicates at the
RPBP layer are therefore a pure application-level concern.

A well-behaved host does not replay frames with the same
`(channel, seq)`. If a device receives a frame whose `seq` is not
exactly one greater than the last received on that channel (modulo
65536), it MUST respond with an `ERROR { status = EPROTO }`. This
catches host-side replay bugs early.

## 16. Versioning

| Bump     | Effect on compatibility                                     |
|----------|-------------------------------------------------------------|
| PATCH    | Clarifications, new golden vectors. Always compatible.      |
| MINOR    | Additive only: new message types, new flags outside reserved, new channels in reserved ranges, new capability keys, new status codes. Forward-compatible. |
| MAJOR    | Breaking changes (header layout, semantics, channel reassignment below 16). Accompanied by a migration guide. |

Devices report their protocol version in `HELLO` and the capability
map's `proto` field. Hosts MUST refuse to operate against a device
whose MAJOR differs from theirs.

## 17. Reference implementations

- Userspace (Rust, canonical): [`userspace/rpbridge-protocol/src/lib.rs`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/userspace/rpbridge-protocol/src/lib.rs)
- Firmware (C): [`firmware/src/rpbp/`](https://github.com/bauer-group/EMB-USBMultiBusBridge/tree/main/firmware/src/rpbp)
- Kernel driver (C): [`driver/src/rpbp_framing.c`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/src/rpbp_framing.c) + [`driver/src/rpbp_crc32c.c`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/src/rpbp_crc32c.c)

All three are exercised by the shared golden-vector corpus under
[`tests/golden/`](https://github.com/bauer-group/EMB-USBMultiBusBridge/tree/main/tests/golden).

## 18. Related documents

- [`capabilities.md`](capabilities.md) — CBOR capability-map schema.
- [`status-codes.md`](status-codes.md) — canonical status byte values.
- [`subsystems/sys.md`](subsystems/sys.md) — SYS subsystem operations.
- [`fragmentation.md`](fragmentation.md) — extended fragmentation rules
  and test vectors.
