# RPBP — LED subsystem (subsys = `0x07`)

Addressable LED strips driven by a PIO state machine on PIO2.
Currently supports WS2812 / WS2812B / SK6812-RGB (3 bytes/pixel,
GRB byte order) plus the WS2811 RGB variant. SK6812-RGBW is
reserved on the wire (4 bytes/pixel) but the v1 firmware sends only
the top 24 bits — the white channel byte is preserved in the
framebuffer for forward-compat.

Access path: vendor RPBP. Linux exposes the universe via `rpbridge.ko`
once an upstream LED-multicolor R4L wrapper exists; until then the
driver attaches the wire client and userspace can talk to the
firmware over the vendor pipe via libusb.

## Capabilities

`buses.led[*]` carries:

| key          | type   | meaning                                |
|--------------|--------|----------------------------------------|
| `idx`        | uint   | universe index (always `0` in v1)     |
| `data_pin`   | uint   | GPIO carrying the strip's data line   |
| `max_pixels` | uint   | compile-time framebuffer ceiling      |
| `chipset`    | text   | `"ws2812"` (advisory)                  |

Feature flag advertised when the build includes LED support:
`led.ws2812`.

## Opcodes

### `0x00` GET_INFO

Request: empty.

Response (8 bytes):

```
+-+-+-+-+-+-+-+-+
|c|p|f|r|pix|max|
+-+-+-+-+-+-+-+-+
```

* `c` (u8) — universe count (1 if available).
* `p` (u8) — GPIO carrying the data line.
* `f` (u8) — current pixel format (`0` GRB, `1` RGB, `2` GRBW).
* `r` (u8) — reserved, must be 0.
* `pix` (LE u16) — currently configured pixel count.
* `max` (LE u16) — compile-time ceiling (`RPBRIDGE_LED_MAX_PIXELS`).

### `0x01` CONFIGURE

Request (4 bytes): `[idx u8][format u8][pixel_count LE u16]`.

* `idx` MUST be `0` (single universe in v1; non-zero → `ENOENT`).
* `format` ∈ `{0, 1, 2}`. Other values → `EINVAL`.
* `pixel_count` ∈ `[1, max_pixels]`. Out of range → `EINVAL`.

The framebuffer is implicitly cleared and a latch pulse is emitted.

### `0x02` WRITE

Request: `[idx u8][offset LE u16][bytes …]`. The byte stream is
copied into the framebuffer at `offset` (in bytes from slot 0). The
firmware shifts the entire frame to the strip after the partial
update and holds the latch pulse for ≥ 50 µs.

Constraints:

* `offset + len` MUST NOT exceed `pixel_count * bytes_per_pixel`.
* Total wire payload ≤ MTU (4 KiB) — chunk into multiple WRITEs for
  larger updates; the framebuffer persists between calls.
* Byte order on the wire matches the configured format. For GRB,
  the canonical pixel triple is `G R B`.

### `0x03` CLEAR

Request: `[idx u8]`. Fills the framebuffer with zeros and latches.

## Examples

### Single red pixel on a 1-LED strip

```
GET_INFO    → max=256, format=GRB, count=1
CONFIGURE   idx=0, format=GRB, pixel_count=1
WRITE       idx=0, offset=0, bytes=[0x00, 0xFF, 0x00]   # G=0 R=255 B=0
```

### 16-LED RGB rainbow

```
CONFIGURE   idx=0, format=GRB, pixel_count=16
WRITE       idx=0, offset=0, bytes=<48 GRB bytes for 16 pixels>
```

### Clear after demo

```
CLEAR       idx=0
```

## Error semantics

| Status     | Cause                                                 |
|------------|-------------------------------------------------------|
| `ENOENT`   | `idx` ≠ 0                                             |
| `EINVAL`   | unknown format, `pixel_count` out of range, oversized |
| `EAGAIN`   | strip not initialised (subsystem disabled at build)   |
| `EBUSY`    | pin/PIO conflict at firmware boot (very rare)         |
| `ENOTSUP`  | board built without `RPBRIDGE_HAVE_LED`               |

## Performance notes

A 256-pixel WS2812 frame takes ~7.7 ms to shift out at 800 kHz
(24 bits × 1.25 µs × 256 + 80 µs latch). The blocking `WRITE` reply
arrives after the latch completes, so a host that issues frames at
> 100 Hz on a 256-pixel strip will saturate the link. For continuous
animation, prefer the partial-WRITE pattern (only the changed bytes)
or step down `pixel_count`.
