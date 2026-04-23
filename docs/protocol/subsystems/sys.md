# RPBP v1 — SYS subsystem

**Subsystem id: `0x00`.** Carries device-level operations that do not
belong to a bus: capability discovery, identification, uptime, LED
control, reboot-to-bootsel, self-test. Every RPBP device MUST
implement every opcode in this document — there is no negotiation
for SYS.

All SYS operations are issued on the control channel (channel `0`)
via `CMD_REQUEST` / `CMD_RESPONSE`. Binary encoding is canonical;
CBOR is permitted and is the default for `GET_CAPABILITIES`.

## Opcode summary

| Opcode | Name             | CBOR-default | Purpose                                  |
|-------:|------------------|:------------:|------------------------------------------|
| `0x00` | `GET_CAPABILITIES` | yes        | Return the CBOR capability map.          |
| `0x01` | `ECHO`           | no           | Round-trip test. Response equals request bytes. |
| `0x02` | `REBOOT_BOOTSEL` | no           | Jump into the ROM USB mass-storage bootloader for OTA. |
| `0x03` | `UPTIME`         | no           | Return microseconds since boot (u64).    |
| `0x04` | `GET_VBUS_MV`    | no           | Return measured VBUS in millivolts (u16).|
| `0x05` | `SET_LED`        | no           | Set the status RGB LED colour + mode.    |
| `0x06` | `SELFTEST`       | no           | Run the on-device loopback self-test.    |
| `0x07` | `GET_IDENTITY`   | yes          | Board / firmware / serial identity.      |
| `0x08` | `RESET`          | no           | Soft-reset the device (re-enumerates).   |

Opcodes `0x09..0x7F` are reserved for additive MINOR-version
extensions. `0x80..0xFF` are vendor-reserved and MUST NOT appear in
upstream firmware.

## `0x00 — GET_CAPABILITIES`

Return the CBOR capability map documented in
[`../capabilities.md`](../capabilities.md). Uses the `CBOR` flag.

**Request (`CBOR` flag set):**

```cbor
{ "s": 0, "o": 0 }
```

**Response (`CBOR` flag set):**

```cbor
{ "s": 0, "o": 0, "st": 0, "r": <capability-map> }
```

Maps larger than `MAX_PAYLOAD_LEN` are transmitted using fragmentation
(see [`../fragmentation.md`](../fragmentation.md)).

## `0x01 — ECHO`

Round-trip test for bring-up and latency measurement.

**Request (binary):**

```text
 0        1        2                            N
 +--------+--------+----------------------------+
 |  0x00  |  0x01  |         payload            |
 +--------+--------+----------------------------+
```

**Response (binary):**

```text
 0        1        2        3                    N
 +--------+--------+--------+--------------------+
 |  0x00  |  0x01  |   OK   |        payload     |
 +--------+--------+--------+--------------------+
```

The device MUST return the `payload` bytes exactly as received. Any
deviation fails the bring-up self-test. Maximum payload size is
`MAX_PAYLOAD_LEN - 4` (to leave room for subsys/opcode/status).

## `0x02 — REBOOT_BOOTSEL`

Request the device to enter the RP2350 ROM USB mass-storage bootloader
so that the host can flash a new UF2 image.

**Request (binary):**

```text
 0        1
 +--------+--------+
 |  0x00  |  0x02  |
 +--------+--------+
```

**Response (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x00  |  0x02  |   OK   |
 +--------+--------+--------+
```

The device MUST send the response **before** triggering the reboot.
The USB reconnection as the RP2350 mass-storage device happens within
~300 ms; the host is expected to tolerate the resulting USB
disconnect. No argument is required — the RP2350 ROM manages its own
activity LED.

## `0x03 — UPTIME`

Return microseconds since device boot as a 64-bit unsigned integer.

**Request (binary):**

```text
 0        1
 +--------+--------+
 |  0x00  |  0x03  |
 +--------+--------+
```

**Response (binary):**

```text
 0        1        2        3                                  10
 +--------+--------+--------+----------------------------------+
 |  0x00  |  0x03  |   OK   |   uptime_us (LE u64)             |
 +--------+--------+--------+----------------------------------+
```

## `0x04 — GET_VBUS_MV`

Read the VBUS voltage through the on-board resistor divider on GP46.
Useful for diagnosing host-side supply issues.

**Request (binary):**

```text
 0        1
 +--------+--------+
 |  0x00  |  0x04  |
 +--------+--------+
```

**Response (binary):**

```text
 0        1        2        3        4        5
 +--------+--------+--------+--------+--------+
 |  0x00  |  0x04  |   OK   | vbus_mv (LE u16) |
 +--------+--------+--------+--------+--------+
```

Accuracy: ±50 mV at the measurement point. A reading outside the
`4500..5500` mV band SHOULD lead the host to warn the user.

## `0x05 — SET_LED`

Control the on-board status RGB LED. Overrides the firmware's own
state-machine colouring until the device is reset.

**Request (binary):**

```text
 0        1        2        3        4        5
 +--------+--------+--------+--------+--------+
 |  0x00  |  0x05  |   R    |   G    |   B    | mode |
 +--------+--------+--------+--------+--------+------+
 | bright |
 +--------+
```

- `R`, `G`, `B` (u8 each) — colour at full brightness.
- `mode` (u8) — `0 = off`, `1 = solid`, `2 = blink 1 Hz`,
  `3 = pulse 1 Hz`, `4 = fast-blink 4 Hz`, `5..255` reserved.
- `bright` (u8) — global brightness 0..100 (%). Values >100 clamped.

**Response (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x00  |  0x05  | status |
 +--------+--------+--------+
```

`status = EINVAL` when `mode` is out of range. `status = OK`
otherwise. To return control to the firmware's own state machine,
send `mode = 0` then wait ~100 ms; the firmware reclaims the LED.

## `0x06 — SELFTEST`

Run an internal suite of loopback and sanity checks.

**Request (binary):**

```text
 0        1        2        3        4        5
 +--------+--------+--------+--------+--------+
 |  0x00  |  0x06  |      test_mask (LE u32)    |
 +--------+--------+-----------------------------+
```

`test_mask` (u32, LE) — bitmap of tests to run. `0xFFFF_FFFF` means
"all available". The capability map's `selftest_tests` array names
each bit.

**Response (binary):**

```text
 0        1        2        3        4        5        6        7
 +--------+--------+--------+--------+--------+--------+--------+--------+
 |  0x00  |  0x06  |   OK   | pass_mask (LE u32)                | fails  |
 +--------+--------+--------+-----------------------------------+--------+
 | (optional per-failure blobs ...)                                      |
 +-----------------------------------------------------------------------+
```

- `pass_mask` (u32, LE) — bits set for tests that passed.
- `fails` (u8) — count of following per-failure blobs.
- Per-failure blob format:
  ```text
  +-------+--------+--------------+--------------------+
  | id u8 | reason (LE u16 len)   | utf-8 up to 255 B  |
  +-------+--------+--------------+--------------------+
  ```

`status != OK` only on catastrophic internal error; test failures
are reported via `pass_mask`, not via `status`.

## `0x07 — GET_IDENTITY`

Shortcut for `GET_CAPABILITIES` when the host only needs
identification strings (board, firmware version, serial). Returns a
compact CBOR map.

**Request (`CBOR` flag):**

```cbor
{ "s": 0, "o": 7 }
```

**Response (`CBOR` flag):**

```cbor
{
  "s": 0, "o": 7, "st": 0,
  "r": {
    "fw":     "0.1.0+abc123",
    "board":  "rpbridge-rev-B",
    "serial": h'1122334455667788',
    "proto":  [1, 0, 0]
  }
}
```

Always fits in a single non-fragmented frame.

## `0x08 — RESET`

Soft-reset the device. Equivalent to toggling the RUN pin: the USB
link drops, the device reboots, and the composite re-enumerates. The
firmware state is discarded (unlike `REBOOT_BOOTSEL`, which starts
the ROM bootloader).

**Request (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x00  |  0x08  |  delay |
 +--------+--------+--------+
```

`delay` (u8) — delay in milliseconds before the reset, 0..200. Allows
the host to receive the response and tear down cleanly before the
link drops.

**Response (binary):**

```text
 0        1        2
 +--------+--------+--------+
 |  0x00  |  0x08  |   OK   |
 +--------+--------+--------+
```

## Error behaviour

Unknown opcodes on subsystem 0 return `CMD_RESPONSE { status = ENOENT }`
with an empty `result_args` section.

Payload-length mismatches for fixed-size requests return
`CMD_RESPONSE { status = EMSGSIZE }`.

CBOR-encoded SYS requests that fail to parse return
`CMD_RESPONSE { status = EPROTO }` in binary form (so even a broken
CBOR request can be diagnosed on the host).
