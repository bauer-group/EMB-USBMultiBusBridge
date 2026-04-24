# GPIO subsystem (subsys = 3)

User-exposed GPIO on the 2×10 breakout header. The firmware keeps a
pin whitelist and refuses requests for any pin outside it; pins that
belong to other subsystems (UART, I²C, SPI, ADC, LED) are already
claimed by the `pin_broker` so even with a matching whitelist they
would return `EBUSY`.

**Whitelist (rev-B)**:

```text
GP30, GP31, GP37, GP38, GP39, GP40, GP41, GP42, GP43, GP44, GP45
(11 pins total)
```

The first six are also PWM-capable; asking the PWM subsystem to take
one of them revokes the GPIO claim atomically (the broker refuses
overlapping ownership).

| Opcode | Name        | Description                                   |
|:------:|:------------|:----------------------------------------------|
| `0x00` | `LIST`      | Enumerate whitelisted pins.                   |
| `0x01` | `CONFIGURE` | Set direction + pull on a pin.                |
| `0x02` | `SET`       | Write a single pin (output only).             |
| `0x03` | `GET`       | Read a single pin.                            |
| `0x04` | `SET_MASK`  | Atomic 64-bit bank write.                     |
| `0x05` | `GET_MASK`  | Atomic 64-bit bank read.                      |
| `0x06` | `WATCH`     | Subscribe to edge events (lands with M6).     |
| `0x07` | `UNWATCH`   | Unsubscribe.                                  |

## 0x00 — LIST

**Args**: none. **Reply body**: `[count u8][pin[count] — GP numbers]`.

## 0x01 — CONFIGURE

**Args** (3 B): `[pin][dir][pull]`.

| Field | Values                                                       |
|:------|:-------------------------------------------------------------|
| dir   | 0 = INPUT, 1 = OUTPUT                                        |
| pull  | 0 = NONE, 1 = UP, 2 = DOWN                                   |

Claims the pin on the broker as `PIN_OWNER_GPIO_USER` with
`PIN_FUNC_GPIO_IN` or `PIN_FUNC_GPIO_OUT`. Idempotent re-configures
by the same subsystem are allowed.

## 0x02 — SET

**Args** (2 B): `[pin][level]`. Returns `EPERM` if the pin is not
configured as OUTPUT by the GPIO subsystem.

## 0x03 — GET

**Args** (1 B): `[pin]`. **Reply body**: `[level u8]`.

## 0x04 — SET_MASK

**Args** (16 B): `[mask LE u64][values LE u64]`.

Pins whose bit in `mask` is set are driven to the corresponding
`values` bit atomically (via the RP2350 SIO `GPIO_OUT_SET` /
`GPIO_OUT_CLR` atomic registers). Bits that don't correspond to
subsystem-owned OUTPUT pins are silently ignored so a host can blast
a wide word without pre-filtering.

## 0x05 — GET_MASK

**Args** (8 B): `[mask LE u64]`. **Reply body**: `[levels LE u64]`
containing `hw_gpio_state & mask`. Unowned bits are zeroed.

## 0x06 / 0x07 — WATCH / UNWATCH

Returned `ENOTSUP` in the M3 build. Edge-triggered event emission on
channel 1 lands with M6 (core 1 + stream channels).

## Capability advertising

```text
buses.gpio[0] = { idx: 0, pins: [30, 31, 37, 38, 39, 40, 41, 42, 43, 44, 45] }
features[] += "gpio.mask-atomic"
```

## Linux userland

`libgpiod` and the `gpiochip` kernel interface work transparently
through the Rust driver — each user-whitelist pin is exposed as an
individual line under a single `gpiochip` node named `rpbridge`.
