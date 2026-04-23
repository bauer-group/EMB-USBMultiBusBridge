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

| `bConfigurationValue` | Name     | Descriptor set                           | Status                     |
|----------------------:|----------|------------------------------------------|----------------------------|
| `1`                   | Compat   | 4× CDC-ACM + 2× Vendor (RPBP, gs_usb)    | **Active / shipping**      |
| `2`                   | Native   | 2× Vendor (RPBP, gs_usb) only            | Reserved — M9 milestone    |
| `3..255`              | —        | —                                        | Available                  |

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

Totals: **10 IN + 6 OUT used, 5 IN + 9 OUT free.**

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

| Block | SMs used | Instr used | Owner(s)                                       |
|-------|---------:|-----------:|------------------------------------------------|
| PIO0  | 0 / 4    | 0 / 32     | **Reserved — can2040 (M5)**                   |
| PIO1  | 2 / 4    | ~11 / 32   | `uart_pio_tx` (≈4 instr), `uart_pio_rx` (≈8 instr) |
| PIO2  | 0 / 4    | 0 / 32     | **Free**                                       |

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
| `can2040`               | 3    | ~32   | PIO0         | M5          | pending   |
| `rs485_de.pio`          | 1    | ~6    | PIO1         | M4d-PIO     | optional  |
| `1-wire`                | 1    | ~14   | PIO2         | future      | planned   |
| `ws2812`                | 1    | ~4    | PIO2         | future      | optional  |
| `i2s_tx` (UAC2 future)  | 1    | ~6    | PIO2         | future      | optional  |
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

The pin broker (M2, pending) owns GPIO assignment at runtime. Until
it lands, the static assignment in
[`firmware/boards/rpbridge_rp2354b.h`](../../firmware/boards/rpbridge_rp2354b.h)
plus [hardware design §6](../drafts/rpbridge-hardware-design.md) is
authoritative. Do not add a second copy of the pin table here —
point at the board header.

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
  (`49416a6`). Reserves Config 2 for M9 driver-priority mode.
