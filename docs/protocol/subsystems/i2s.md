# RPBP — I²S subsystem (subsys = `0x0A`)

Philips I²S transmitter implemented on a single PIO state machine
on PIO2. 16-bit signed PCM stereo from 8 kHz to 96 kHz. TX-only in
v1; RX is a future extension.

The PIO program drives BCLK + LRCLK via side-set on consecutive
pins and shifts data MSB-first via OUT. SM clock = 64 × sample
rate, so 32 BCLKs per L/R frame. Standard "data delayed by one
BCLK" Philips framing — works directly with PCM5102A,
PCM5101, MAX98357, ES9023, and any other DAC that accepts
32-fs / 16-bit stereo I²S.

Hardware: three GPIOs — BCLK, LRCLK (= BCLK + 1, enforced at build
time), and DOUT. The DAC's MCLK is **not** generated; if your DAC
needs an MCLK input, it must be supplied externally (a DAC like
PCM5102A operates without an MCLK if the option pins are strapped
correctly).

Access path: vendor RPBP. There is no kernel ASoC adaptation yet —
ALSA / ASoC integration is genuinely heavyweight (machine driver +
codec driver + DAI link) and tracked separately in driver/TODO.md
§M12. Userspace can drive the firmware directly via libusb in the
meantime.

v1 scope is **buffer playback** ("play this set of frames") rather
than continuous streaming. WRITE blocks until the FIFO accepts the
buffer; the SM then clocks it out at the configured rate.
Continuous multi-second playback needs an RPBP STREAM_DATA channel
— follow-up.

## Capabilities

`buses.i2s[*]` carries:

| key         | type   | meaning                                  |
|-------------|--------|------------------------------------------|
| `idx`       | uint   | TX index (always `0` in v1)              |
| `bclk_pin`  | uint   | bit-clock GPIO                           |
| `lrclk_pin` | uint   | word-select GPIO (= `bclk_pin` + 1)      |
| `dout_pin`  | uint   | serial-data-out GPIO                     |
| `min_rate`  | uint   | lowest supported sample rate (Hz)        |
| `max_rate`  | uint   | highest supported sample rate (Hz)       |
| `direction` | text   | `"tx"` (v1)                              |

Feature flag: `i2s.tx`.

## Opcodes

### `0x00` GET_INFO

Request: empty.

Response (12 bytes):

```
+-+-+-+-+-+-+-+-+-+-+-+-+
|c|B|L|D|sample_rate|d|r|R|R|
+-+-+-+-+-+-+-+-+-+-+-+-+
```

* `c` (u8) — TX count.
* `B`, `L`, `D` (u8 each) — BCLK, LRCLK, DOUT pins.
* `sample_rate` (LE u32) — currently configured rate (Hz).
* `d` (u8) — bit depth (always 16 in v1).
* `r` (u8) — running flag (0/1).
* `R` (u8 ×2) — reserved, must be 0.

### `0x01` CONFIGURE

Request (5 bytes): `[idx u8][sample_rate LE u32]`.

* `sample_rate` ∈ `[8000, 96000]`. Out of range → `EINVAL`.
* If the SM was running, it is stopped and the FIFO drained before
  the new rate takes effect — the host must explicitly `START`
  again.

### `0x02` START

Request: `[idx u8]`. Begins clocking the FIFO out. Idempotent.

### `0x03` STOP

Request: `[idx u8]`. Drains pending FIFO bytes (so playback ends at
a frame boundary), then disables the SM. Idempotent.

### `0x04` WRITE

Request: `[idx u8][stereo PCM frames …]`.

Each frame is **4 bytes** in this layout: `L_lo, L_hi, R_lo, R_hi`
(little-endian signed 16-bit per channel). The driver translates to
the PIO wire format internally.

* `frames.len()` MUST be a non-zero multiple of 4 → `EINVAL`
  otherwise.
* Total wire payload ≤ MTU (4 KiB) → max ~1023 stereo frames per
  WRITE (= ~21 ms at 48 kHz). Chunk larger payloads across
  successive WRITEs; the SM keeps reading the FIFO across calls.

## Examples

### Play a 1 kHz sine for 100 ms at 48 kHz

```
CONFIGURE  idx=0, sample_rate=48000
START      idx=0
loop:
    WRITE  idx=0, frames=<chunk of stereo PCM>     # ≤ 4 KiB per call
STOP       idx=0
```

### Switch sample rate mid-playback

```
STOP
CONFIGURE  idx=0, sample_rate=44100
START
WRITE      ...
```

## Error semantics

| Status     | Cause                                                |
|------------|------------------------------------------------------|
| `ENOENT`   | `idx` ≠ 0                                            |
| `EINVAL`   | rate out of range, frames not multiple of 4, empty   |
| `EAGAIN`   | TX not initialised (subsystem disabled at build)     |
| `EBUSY`    | pin/PIO conflict at firmware boot                    |
| `ENOTSUP`  | board built without `RPBRIDGE_HAVE_I2S`              |

## Hardware wiring (PCM5102A — typical bare-board DAC)

```
RP2350  GP41 ───► BCK   (PCM5102A pin 13)
        GP42 ───► LRCK  (PCM5102A pin 15)
        GP45 ───► DIN   (PCM5102A pin 14)
        GND  ───► GND
        +3V3 ───► VIN, AVIN  (or +5V → 3V3 LDO on the module)

PCM5102A option pins (set on the breakout board):
        SCK   = GND   (no external MCLK; internal PLL handles it)
        FMT   = GND   (Philips I²S, not left-justified)
        XSMT  = +3V3  (un-mute)
```

Connect the line-out (LOUTL, LOUTR, AGND) to powered speakers or
a small headphone amp. Decoupling: 10 µF + 100 nF on AVIN, 10 µF
+ 100 nF on VIN.

## Performance notes

USB FS bulk endpoints sustain ~1 MB/s in practice. 48 kHz × 16-bit
stereo is 192 kB/s (1.5 Mbit/s) of audio data, so a single bulk
endpoint can carry the stream. The 4 KiB MTU caps each WRITE at
~21 ms of audio at 48 kHz, which sets the upper bound on WRITE
cadence the host must sustain. For continuous multi-second
playback in v1, the host must keep a back-pressure-aware buffer
and call WRITE every ~10 ms; v2 will replace this with a STREAM
channel and drop the call cadence requirement.
