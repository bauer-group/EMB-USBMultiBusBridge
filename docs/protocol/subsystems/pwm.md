# PWM subsystem (subsys = 4)

PWM generation on the user header. The RP2350 has 12 PWM slices ×
2 channels; this subsystem exposes six of those channels on the
2×10 breakout (GP30, GP31, GP37, GP38, GP39, GP40). The status-LED
channels (GP22/23/24) are PWM-driven too but reserved for the
firmware — they are **not** addressable via this subsystem.

Duty is a Q0.16 fraction (`duty_q16 ∈ [0, 65535]`): 0 = 0 %,
`0xFFFF` = 100 %, `0x8000` ≈ 50 %.

Frequency is a positive integer in Hz. The firmware picks the smallest
achievable clock divider so the counter wrap has the highest possible
resolution; hosts can read back the **actually applied** frequency via
`GET_STATE` to cope with quantisation.

| Opcode | Name        | Description                                  |
|:------:|:------------|:---------------------------------------------|
| `0x00` | `CONFIGURE` | pin + freq + duty + flags; starts the slice. |
| `0x01` | `SET_DUTY`  | Update duty on a pin already configured.     |
| `0x02` | `START`     | Resume the slice that owns `pin`.            |
| `0x03` | `STOP`      | Freeze the slice (output goes to idle).      |
| `0x04` | `GET_STATE` | Query applied freq/duty/running/slice info.  |

## 0x00 — CONFIGURE

**Args** (8 B): `[pin][flags][duty_q16 LE u16][freq_hz LE u32]`.

Flags:

| Bit | Name     | Meaning                                      |
|:---:|:---------|:---------------------------------------------|
|  0  | `INVERT` | Invert output polarity at the GPIO override  |

Returns `ENOENT` if `pin` is not on the PWM whitelist, `EINVAL` on
zero / unreachable frequency, `EBUSY` if another pin on the same
slice is already configured with a different frequency (slices share
their wrap register).

## 0x01 — SET_DUTY

**Args** (3 B): `[pin][duty_q16 LE u16]`.

Returns `ENOENT` if the pin has not been configured.

## 0x02 / 0x03 — START / STOP

**Args** (1 B): `[pin]`. Enables or disables the PWM slice that owns
`pin`. `STOP` preserves the configured parameters; `START` resumes
with the same freq and duty. When two pins share a slice, both are
affected.

## 0x04 — GET_STATE

**Args** (1 B): `[pin]`.

**Reply body** (10 B):

```text
[0..3]   freq_hz_actual   LE u32
[4..5]   duty_q16         LE u16
[6]      running          (0 | 1)
[7]      invert           (0 | 1)
[8]      slice            (0..11)
[9]      channel          (0 = A, 1 = B)
```

## Capability advertising

```text
buses.pwm[0] = { idx: 0, max_freq: 62500000, pins: [30, 31, 37, 38, 39, 40] }
features[] += "pwm.invert"
```

## Linux userland

Each user PWM pin maps to a `/sys/class/pwm/pwmchip<N>/pwmX` node
via the standard `pwm-chip` driver abstraction. The Rust driver
registers one chip per SPI-style bank so `pwm_get(&dev, 0)` resolves
to GP30, `pwm_get(&dev, 1)` to GP31, etc.
