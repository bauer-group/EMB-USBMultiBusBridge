# Firmware — shared-resource ownership tracker

**Single source of truth for every scarce RP2354B resource the
firmware carves up.** Whenever a module takes a slice (a PIO state
machine, a USB endpoint, a DMA channel, a GPIO pin, a USB
Configuration slot), update this file **in the same pull request**.
A reviewer who reads only this page must be able to tell what is
claimed, what is free, and what is earmarked for a named milestone.

The numbers here are the authoritative budget — code comments like
"PIO0 is reserved for can2040" point **at** this page, not the
other way round.

---

## 1. USB Configurations

USB 2.0 lets a composite device advertise several configurations;
the host picks exactly one at a time with `SET_CONFIGURATION`. We
use that to offer a driverless-compatible descriptor set today and
reserve room for a future driver-native set. See
[ADR-0004](../adr/0004-driver-priority-mode.md) for the full
rationale.

| `bConfigurationValue` | Name     | Descriptor set                                | Status                     |
|----------------------:|----------|-----------------------------------------------|----------------------------|
| `1`                   | Compat   | 4× CDC-ACM + 2× Vendor (RPBP, gs_usb) †       | **Active / shipping**      |
| `2`                   | Native   | 2× Vendor (RPBP, gs_usb) only                 | Reserved — M9 milestone    |
| `3..255`              | —        | —                                             | Available                  |

† — A 5th CDC-ACM (`"RPBridge Debug Log"`) can be enabled as a
build-time option (`-DRPBRIDGE_DEBUG_CDC=ON`, default OFF). When
ON, it adds CDC #4 at `bInterfaceNumber {8,9}` and shifts the two
vendor interfaces from `{8,9}` to `{10,11}`. Configuration value,
EP allocation for #0..#3, and the vendor EPs are unchanged. See
the #Config 1 EP table for the alternate row, and
[`docs/usb-vid-pid.md`](../usb-vid-pid.md) for the descriptor view.

Rules:

- Config 1 is the always-on default. Never renumber it.
- A new configuration must be added here **before** its descriptor
  gets wired into `usb_descriptors.c`.
- `bNumConfigurations` in `desc_device` must match the count of
  active rows above.

---

## 2. USB Endpoints

RP2350's USB controller offers **15 non-zero endpoints per
direction** (IN 1..15 + OUT 1..15) plus EP0 (shared bidirectional
control). Running inventory:

### Config 1 (Compat) — current

| Endpoint   | Direction | Size  | Type       | Owner                                    |
|-----------:|-----------|------:|------------|------------------------------------------|
| EP0        | IN + OUT  |   64  | Control    | TinyUSB core + MS OS 2.0 vendor requests |
| EP1 IN     | IN        |    8  | Interrupt  | CDC-ACM #0 notification                  |
| EP2 OUT    | OUT       |   64  | Bulk       | CDC-ACM #0 data                          |
| EP2 IN     | IN        |   64  | Bulk       | CDC-ACM #0 data                          |
| EP3 IN     | IN        |    8  | Interrupt  | CDC-ACM #1 notification                  |
| EP4 OUT    | OUT       |   64  | Bulk       | CDC-ACM #1 data                          |
| EP4 IN     | IN        |   64  | Bulk       | CDC-ACM #1 data                          |
| EP5 IN     | IN        |    8  | Interrupt  | CDC-ACM #2 notification                  |
| EP6 OUT    | OUT       |   64  | Bulk       | CDC-ACM #2 data                          |
| EP6 IN     | IN        |   64  | Bulk       | CDC-ACM #2 data                          |
| EP7 IN     | IN        |    8  | Interrupt  | CDC-ACM #3 notification                  |
| EP8 OUT    | OUT       |   64  | Bulk       | CDC-ACM #3 data                          |
| EP8 IN     | IN        |   64  | Bulk       | CDC-ACM #3 data                          |
| EP9 OUT    | OUT       |   64  | Bulk       | Vendor RPBP control                      |
| EP9 IN     | IN        |   64  | Bulk       | Vendor RPBP control                      |
| EP10 OUT   | OUT       |   64  | Bulk       | Vendor gs_usb CAN                        |
| EP10 IN    | IN        |   64  | Bulk       | Vendor gs_usb CAN                        |
| EP11 IN ‡  | IN        |    8  | Interrupt  | CDC-ACM #4 notification (debug log)      |
| EP12 OUT ‡ | OUT       |   64  | Bulk       | CDC-ACM #4 data         (debug log)      |
| EP12 IN ‡  | IN        |   64  | Bulk       | CDC-ACM #4 data         (debug log)      |

‡ — present only when `-DRPBRIDGE_DEBUG_CDC=ON`. The vendor EPs
(EP9, EP10) are *not* renumbered when the flag flips — only the
interface numbers shift. This keeps host-side code that walks bulk
endpoints stable across build variants.

Totals: **10 IN + 6 OUT used (default), 12 IN + 7 OUT used with
the debug CDC; 5 IN + 9 OUT (resp. 3 IN + 8 OUT) free for future
streaming pipes.**

### Config 2 (Native) — reserved for M9

| Endpoint  | Direction | Size  | Type  | Owner                                 |
|----------:|-----------|------:|-------|---------------------------------------|
| EP0       | IN + OUT  |   64  | Ctrl  | unchanged                             |
| EP1 OUT   | OUT       |   64  | Bulk  | Vendor RPBP (UART streams + control)  |
| EP1 IN    | IN        |   64  | Bulk  | Vendor RPBP                           |
| EP2 OUT   | OUT       |   64  | Bulk  | Vendor gs_usb CAN                     |
| EP2 IN    | IN        |   64  | Bulk  | Vendor gs_usb CAN                     |

Totals: **2 IN + 2 OUT used, 13 IN + 13 OUT free for future
streaming pipes (logic capture, IIO trigger, audio UAC2, …).**

---

## 3. PIO blocks, state machines, instructions

RP2350 = 3 PIO blocks × 4 SMs × 32 instruction slots = **12 SMs /
96 instructions** total. Instruction memory is per-block;
programs cannot cross block boundaries.

Ring allocation policy:

- **PIO0** — hard-reserved for `can2040`. Do not place other
  applets here, even if SMs look free; `can2040` expects exclusive
  control of the block's IRQs and instruction memory.
- **PIO1** — user-UART tier. PIO-UART #2 currently lives here;
  RS485 precision-DE would move here when we stop relying on the
  software timer.
- **PIO2** — optional applets (1-Wire, WS2812, I²S, logic capture).
  First-come-first-served among "future nice-to-haves".

### Today

| Block | SMs used | Instr used | Owner(s)                                                                  |
|-------|---------:|-----------:|---------------------------------------------------------------------------|
| PIO0  | 3 / 4    | 32 / 32    | **can2040 (M5)** — block-exclusive                                        |
| PIO1  | 3 / 4    | ~25 / 32   | `uart_pio_tx` (≈4), `uart_pio_rx` (≈8), `onewire` (≈14)                   |
| PIO2  | 3 / 4    | ~17 / 32   | `ws2812` (≈4), `dmx_tx` (≈5), `i2s_tx` (≈8)                               |

### After all planned applets land

| Block | SMs used | Instr used | Owner(s)                                                |
|-------|---------:|-----------:|---------------------------------------------------------|
| PIO0  | 3 / 4    | 32 / 32    | can2040 (sync, rx, tx) — M5                             |
| PIO1  | 3 / 4    | ~17 / 32   | `uart_pio_tx`, `uart_pio_rx`, `rs485_de.pio` (M4d-PIO)  |
| PIO2  | ~4 / 4   | ~30 / 32   | 1-Wire, WS2812, I²S-TX, Logic-Capture (mixed)           |

**Reserve at worst case**: ~2 SMs + ~50 instructions across blocks.
A PR that pushes PIO2 above 28 instr MUST coordinate with other
planned PIO2 consumers or push to a second-instance PIO2 applet
that's no longer in v1 scope.

### Pending applets and their cost estimates

| Applet                  | SMs  | Instr | Target block | Milestone   | Status    |
|-------------------------|-----:|------:|--------------|-------------|-----------|
| `can2040`               | 3    | ~32   | PIO0         | M5          | landed    |
| `rs485_de.pio`          | 1    | ~6    | PIO1         | M4d-PIO     | optional  |
| `onewire`               | 1    | ~14   | PIO1         | M12         | landed    |
| `ws2812`                | 1    | ~4    | PIO2         | M12         | landed    |
| `dmx_tx`                | 1    | ~5    | PIO2         | M12         | landed    |
| `i2s_tx`                | 1    | ~8    | PIO2         | M12         | landed    |
| `logic_capture`         | 1-4  | ~20   | PIO2 / PIO1  | future      | planned   |
| Extra PIO-UART #2..n    | 2/ea | ~11   | PIO1-rest    | future      | optional  |

Any row labelled "pending" blocks the associated milestone; any row
labelled "optional" can be skipped without affecting downstream
work.

---

## 4. DMA channels

RP2350 has **16 DMA channels** shared across all peripherals.
Allocation is dynamic through `dma_claim_unused_channel()` — no
fixed-channel reservations today. As subsystems land we track
expected steady-state usage here:

| Peripheral / use case       | Typical channels | Pattern                |
|-----------------------------|-----------------:|------------------------|
| USB bulk (TinyUSB)          | 0 (polled)       | Polled service         |
| I²C DMA burst (M2)          | 2                | TX + RX paired         |
| SPI DMA burst (M3)          | 2                | TX + RX paired         |
| ADC round-robin (M3)        | 1                | Ring-buffered stream   |
| can2040 (M5)                | 0                | Core-bound / PIO IRQ   |
| Reserved for logic capture  | 1-2              | Future                 |

Expected worst-case total: ~6 channels. **10 channels free** at
M5 completion. No capacity concern.

---

## 5. GPIO ownership

The pin broker
([`firmware/src/peripherals/pin_broker.{c,h}`](../../firmware/src/peripherals/pin_broker.c))
owns the runtime ledger. Every module that takes a pin MUST call
`pin_broker_claim()` during init; overlapping claims return
`-RPBP_EBUSY` and fail the init. The board header
[`firmware/boards/rpbridge_rp2354b.h`](../../firmware/boards/rpbridge_rp2354b.h)
remains the static reference and the authoritative pinout (it
matches [hardware-design §6](../drafts/rpbridge-hardware-design.md)).

Live claims as of M3:

| Pin(s)              | Owner          | Function                                                   |
|---------------------|----------------|------------------------------------------------------------|
| GP0, GP1            | `uart-hw`      | UART0 TX / RX (phy-bank backend)                           |
| GP2, GP3            | `i2c`          | I²C #0 SDA / SCL (Qwiic 1+2)                               |
| GP4, GP5            | —              | UART1 TX / RX (owned by pico-sdk stdio_uart)               |
| GP6                 | `uart-phy`     | RS485 DE (handed to uart_hw while in RS485 mode)           |
| GP7                 | `uart-phy`     | RS485 EN                                                   |
| GP8                 | `uart-phy`     | RS232 /SHDN                                                |
| GP9                 | free           | SPI0 user IRQ (lazy claim — see cmd_spi + docs)            |
| GP10..GP12          | `spi`          | SPI1 SCK / MOSI / MISO                                     |
| GP13..GP15          | `spi` (lazy)   | SPI1 CS0 / CS1 / CS2 (claimed on first XFER)               |
| GP16                | `spi`          | SPI0 MISO                                                  |
| GP17                | `spi` (lazy)   | SPI0 CS (claimed on first XFER)                            |
| GP18, GP19          | `spi`          | SPI0 SCK / MOSI                                            |
| GP20, GP21          | `can`          | CAN 2.0B TX / RX (can2040 on PIO0)                         |
| GP22..GP24          | `system`+`pwm` | RGB status LED (PWM-driven, 1 kHz)                         |
| GP26..GP29          | `adc`          | ADC0..ADC3 (user header A0..A3)                            |
| GP30, GP31          | `gpio`/`pwm`   | User header — lazy claim on CONFIGURE                      |
| GP32                | `1-wire`       | 1-Wire data line (PIO1 SM 2; external 4.7 kΩ pull-up)      |
| GP33, GP36          | `uart-pio`     | PIO-UART TX / RX                                           |
| GP34, GP35          | `i2c`          | I²C #1 SDA / SCL (Qwiic 3)                                 |
| GP37..GP40          | `gpio`/`pwm`   | User header — lazy claim on CONFIGURE                      |
| GP41                | `system`       | I²S BCLK (PIO2 SM 2)                                       |
| GP42                | `system`       | I²S LRCLK (PIO2 SM 2; must be BCLK + 1)                    |
| GP43                | `system`       | WS2812 / NeoPixel data line (PIO2 SM 0)                    |
| GP44                | `system`       | DMX512 TX (to external RS-485 transceiver, PIO2 SM 1)      |
| GP45                | `system`       | I²S DOUT (PIO2 SM 2)                                       |

The pico2 variant overlays the PIO2 / PIO1 applet pins onto its
narrower GP8..GP15 user range:

- `RPBRIDGE_LED_STRIP_PIN`  = GP14
- `RPBRIDGE_DMX_TX_PIN`     = GP15
- `RPBRIDGE_ONEWIRE_PIN`    = GP12
- `RPBRIDGE_I2S_BCLK_PIN`   = GP8 (LRCLK on GP9)
- `RPBRIDGE_I2S_DOUT_PIN`   = GP13

See `firmware/boards/rpbridge_pico2.h`.

Pending subsystems (logic capture, RS485 precision-DE, RX
extensions for 1-Wire / DMX) will land in future milestones.
Update this table in the same PR that wires each one in.

The broker is introspectable at runtime via
`pin_broker_snapshot()`; future `SYS SELFTEST` payloads and
`rpbridge-ctl pin list` consume that snapshot so developers can
diagnose conflicts without flashing a debug image.

---

## 6. Procedure for allocating new resources

1. Open this file and find the relevant table.
2. Mark the row you intend to consume.
3. Implement. The code comment that introduces the consumer SHOULD
   reference this doc by stable anchor, not by duplicating the
   rationale.
4. In the same commit, **add a log entry** in the changelog below
   so history stays legible.

If you discover a conflict (two PRs claiming the same SM, for
example) resolve it here first, in the tracker, before touching
code — the earliest-PR-wins rule applies.

---

## Changelog

- 2026-04-23 — doc introduced. Captures current USB config,
  endpoint, PIO, DMA and GPIO state as of the M4e commit
- 2026-04-23 — M3 landed. SPI (GP10..19), ADC (GP26..29), PWM-driven
  RGB LED (GP22..24), plus the user-header lazy-claim zone
  (GP30/31/37..45). No new PIO or DMA usage — all HW-backed.
  (`49416a6`). Reserves Config 2 for M9 driver-priority mode.
- 2026-04-25 — M12 PIO2 applets landed. WS2812 (`led_strip`, PIO2 SM 0,
  ~4 instr) on GP43 (rev-B) / GP14 (pico2); DMX512-A TX (`dmx`, PIO2
  SM 1, ~5 instr) on GP44 (rev-B) / GP15 (pico2). Subsystem IDs
  `0x07` LED + `0x08` DMX.
- 2026-04-25 — M12 PIO1 + PIO2 expansion. 1-Wire master (`onewire`,
  PIO1 SM 2, ~14 instr) on GP32 (rev-B) / GP12 (pico2); I²S TX
  (`i2s`, PIO2 SM 2, ~8 instr) on GP41/42/45 (rev-B) / GP8/9/13
  (pico2). Final PIO budget: PIO0 sealed, PIO1 3/4 SMs ~25/32 instr,
  PIO2 3/4 SMs ~17/32 instr. Subsystem IDs `0x09` ONEWIRE + `0x0A`
  I²S.
