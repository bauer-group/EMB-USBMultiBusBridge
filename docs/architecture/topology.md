# System topology

See the ASCII topology diagram in [`overview.md`](overview.md). Per-
interface details:

- **USB composite** — 4× CDC-ACM (UART1 × 3 transceiver banks
  RS232 / RS485 / TTL, plus UART2 via PIO), 1× vendor/WinUSB
  (RPBP transport), 1× vendor/gs_usb (CAN). Debug log is not on
  a CDC — `stdio` routes to UART1 hardware on the 5-pin debug
  header (GP4/GP5).
- **Pin broker** — firmware-side authority for GPIO ownership. Every
  subsystem requests pins through the broker; conflicts fail with
  `EBUSY`.
- **DMA map** — long-running peripherals (SPI bursts, ADC streaming)
  reserve dedicated DMA channels during capability emit so the host
  sees them as available before allocation.

## Clock domains

| Domain            | Source                               | Notes                          |
|-------------------|--------------------------------------|--------------------------------|
| `clk_sys`         | PLL from 12 MHz XO, 150 MHz target   | Drives both cores + PIO.       |
| `clk_peri`        | 125 MHz                              | Peripheral SPI/I²C/UART ref.   |
| `clk_usb`         | 48 MHz PLL                           | TinyUSB timing.                |
| `clk_adc`         | 48 MHz                               | SAR ADC.                       |

## Memory map highlights

- SRAM bank 0: stacks for both cores.
- SRAM bank 1: TinyUSB buffers (4-byte aligned, kept on one bank to
  avoid DMA contention).
- SRAM bank 2: DMA descriptors + peripheral ring buffers.
- SRAM banks 3–8: free for RPBP queues and application payloads.
- Flash: single firmware image; no A/B partitions in v1.

For the full elaboration see
[`docs/drafts/rpbridge-architektur.md`](../drafts/rpbridge-architektur.md) §4.5.
