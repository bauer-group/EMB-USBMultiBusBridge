# RPBP v1 — capability map

The device answers `CMD_REQUEST{ GET_CAPABILITIES }` with a CBOR map
that advertises the protocol version, firmware identity, per-bus
limits and the dynamic channel assignments. The host MUST use this
map to gate feature use — **never** hard-code behaviour by firmware
version.

## Top-level keys

| Key              | Type          | Purpose                                          |
|------------------|---------------|--------------------------------------------------|
| `proto`          | array[u8;3]   | `[MAJOR, MINOR, PATCH]` of the RPBP spec.        |
| `fw`             | text          | Firmware version (`"1.2.0+abc123"`).             |
| `fw_git`         | text          | Short git hash of the firmware build.            |
| `fw_built`       | text          | ISO-8601 build timestamp.                        |
| `board`          | text          | Board identifier (`"rpbridge-rev-B"`).           |
| `board_rev`      | text          | PCB revision letter.                             |
| `hw_uid`         | bstr          | RP2350 flash unique id, 8 bytes.                 |
| `mtu`            | map           | `{out: u16, in: u16}` — bulk frame MTU.          |
| `max_streams`    | u16           | Maximum simultaneous dynamic streams.            |
| `features`       | array[text]   | Free-form feature tags (see below).              |
| `buses`          | map           | Per-bus descriptors (see below).                 |
| `access`         | map           | Per-subsystem routing — see "Access map" below.  |
| `channels`       | map           | Initial static channel assignments.              |
| `max_rx_inflight`| map           | Initial credit budget, per channel.              |
| `ota`            | map           | OTA mechanism + signing key fingerprint.         |
| `identity`       | map           | Serial, manufacturer, OTP flags.                 |

## Example

```json
{
  "proto": [1, 0, 0],
  "fw":    "0.1.0+a1b2c3d",
  "board": "rpbridge-rev-B",
  "mtu":   { "out": 512, "in": 512 },
  "features": [
    "uart.rs232", "uart.rs485", "uart.ttl",
    "can.classic",
    "i2c.100k", "i2c.400k", "i2c.1m",
    "spi.mode0..3",
    "gpio.input", "gpio.output", "gpio.irq",
    "adc.12bit",
    "crc32c.hw", "cbor", "credit-fc",
    "time-sync", "events",
    "secure-boot", "firmware-update"
  ],
  "buses": {
    "i2c": [
      { "idx": 0, "max_freq": 1000000 },
      { "idx": 1, "max_freq":  400000 }
    ],
    "spi": [
      { "idx": 0, "max_freq": 31250000, "modes": [0,1,2,3] }
    ],
    "gpio": { "count": 14, "pwm_capable": [30,31,37,38,39], "adc_channels": [26,27,28,29] }
  },
  "max_rx_inflight": { "16": 8192, "48": 4096, "80": 256 }
}
```

## Access map

The `access` top-level key tells the host **where each subsystem
is reachable from**. Introduced in
[ADR-0005](../adr/0005-driver-optional-coexistence.md) together
with the single-configuration coexistence model.

Value is a map from subsystem-class name to one of:

| Value         | Meaning                                                               |
|:--------------|:----------------------------------------------------------------------|
| `"cdc"`       | Reachable only via the inbox `usbser`-bound CDC-ACM virtual COM port. |
| `"vendor"`    | Reachable only via RPBP on the vendor interface (driver or libusb).    |
| `"cdc+vendor"` | Reserved for future dual-reachable subsystems.                        |

Example for rev-B firmware (default):

```cbor
"access": {
  "uart": "cdc",
  "i2c":  "vendor",
  "spi":  "vendor",
  "gpio": "vendor",
  "pwm":  "vendor",
  "adc":  "vendor",
  "can":  "vendor"
}
```

Hosts use this field to route subsystem requests to the correct
endpoint without guessing — e.g. the Rust driver's `Capabilities`
parser folds the access tags directly into each `BusCaps` entry so
the i2c-adapter child knows "send XFER down the vendor pipe" and
the (future) tty child knows "speak to the CDC". Unknown subsystem
keys MUST be ignored per the forward-compatibility contract below.

## Feature-tag conventions

Feature strings are short, lowercase, dot-separated. Examples in
active use:

- `crc32c.hw` — device uses the RP2350 DMA SNIFFER block for CRC.
- `compression.lz4` — payload compression supported.
- `cbor` — CBOR payloads accepted on control channel.
- `credit-fc` — stream credit flow control honoured.
- `time-sync` — `TIME_SYNC` message understood.
- `secure-boot` — firmware image was signed-boot-verified.

New features land without a protocol-version bump; old hosts simply
don't see the string.

## Forward-compatibility contract

- Hosts MUST ignore unknown keys.
- Firmware MUST NOT remove keys without a MAJOR version bump.
- Hosts MUST NOT reject devices on `MINOR_host < MINOR_device` —
  they simply do not use unknown subsystems.
