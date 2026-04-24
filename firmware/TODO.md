# Firmware — implementation roadmap

Track for each component whether it is implemented (`[x]`) or pending
(`[ ]`). Every entry points to the corresponding module and the
authoritative spec section.

## M1 — RPBP v1 end-to-end

- [x] `src/rpbp/rpbp.h` — protocol constants (msg_type, flags,
      channels, status codes, SYS opcodes)
- [x] `src/rpbp/rpbp_framing.{c,h}` — header encode/parse, frame
      build/validate, reserved-flag-bit check
- [x] `src/rpbp/rpbp_crc32c.{c,h}` — Castagnoli CRC-32C lookup table
- [x] `src/rpbp/rpbp_cbor.{c,h}` — minimal RFC 8949 writer
- [x] `src/rpbp/rpbp_dispatch.{c,h}` — msg_type router + reply
      builders (CMD_RESPONSE, ERROR, PONG, EVENT)
- [x] `src/rpbp/rpbp_transport.{c,h}` — TinyUSB vendor RX/TX state
      machine with resync
- [x] `src/cap/capabilities.{c,h}` — CBOR capability + identity emitters
- [x] `src/commands/cmd_sys.{c,h}` — SYS subsystem handlers (all 9 opcodes
      from `docs/protocol/subsystems/sys.md`)
- [x] `src/main.c` — TinyUSB + transport pump in the main loop
- [x] `tests/host/` — host-side unit tests for CRC, framing, CBOR

## M2 — HW-I²C + pin broker

- [x] `src/peripherals/pin_broker.{c,h}` — 48-slot ownership ledger.
      Conflicts return `-RPBP_EBUSY`; idempotent re-claims by the
      same owner update the function tag. Existing modules
      (`uart_hw`, `uart_pio`, `uart_phy`, LED init) claim through
      the broker; future subsystems must do the same.
- [x] `src/peripherals/i2c_hw.{c,h}` — `hardware_i2c` wrapper with
      timeout-bounded blocking transfers. Supports 100 k / 400 k /
      1 M baud, write / read / write-restart-read patterns, and
      the pin broker. DMA path deferred to M6 (no real consumer
      needs it before streaming lands).
- [x] `src/commands/cmd_i2c.{c,h}` — RPBP opcodes `PROBE (0x00)`,
      `XFER (0x01)`, `SCAN (0x02)`, `SET_FREQ (0x03)`,
      `GET_FREQ (0x04)`; spec at
      [`docs/protocol/subsystems/i2c.md`](../docs/protocol/subsystems/i2c.md).
- [x] Capability map advertises two I²C instances under
      `buses.i2c` plus `"i2c.100k" / .400k / .1m"` feature flags.
- [x] `main.c` brings up `pin_broker_init()` first, then calls
      `i2c_hw_init_all()` after the USB stack is up.

## M3 — SPI + GPIO + PWM + ADC

- [x] `src/peripherals/spi.{c,h}` + `src/commands/cmd_spi.c` —
      dual-instance PL022 wrapper (SPI0 on PMOD, SPI1 on breakout
      with 3 CS lines). Manual software CS. Opcodes `XFER` /
      `SET_MODE` / `SET_FREQ` / `GET_FREQ` / `CS_ASSERT` /
      `CS_RELEASE`. Spec at
      [`docs/protocol/subsystems/spi.md`](../docs/protocol/subsystems/spi.md).
- [x] `src/peripherals/gpio.{c,h}` + `src/commands/cmd_gpio.c` —
      11-pin user whitelist (GP30-31, GP37-45). Opcodes `LIST` /
      `CONFIGURE` / `SET` / `GET` / `SET_MASK` / `GET_MASK` (64-bit
      atomic). `WATCH` / `UNWATCH` reserved for M6 (core 1 + events
      channel). Spec at
      [`docs/protocol/subsystems/gpio.md`](../docs/protocol/subsystems/gpio.md).
- [x] `src/peripherals/pwm.{c,h}` + `src/commands/cmd_pwm.c` —
      6-pin user whitelist (GP30, GP31, GP37-40) via the 12 PWM
      slices. Opcodes `CONFIGURE` / `SET_DUTY` / `START` / `STOP`
      / `GET_STATE`. Duty is Q0.16; per-slice frequency conflict
      returns `EBUSY`. Spec at
      [`docs/protocol/subsystems/pwm.md`](../docs/protocol/subsystems/pwm.md).
- [x] `src/peripherals/adc.{c,h}` + `src/commands/cmd_adc.c` —
      4 external channels (GP26-29) + on-chip temperature sensor
      with datasheet-calibrated decoding. Single-shot only;
      streaming / IIO lands with M6.
      Opcodes `READ` / `READ_MANY` / `GET_REF` / `TEMP_READ`.
      Spec at
      [`docs/protocol/subsystems/adc.md`](../docs/protocol/subsystems/adc.md).
- [x] LED brightness via PWM instead of binary GPIO in `main.c` —
      1 kHz PWM on GP22/23/24 with per-channel perceptual
      calibration (30 % R / 100 % G / 60 % B). Default heartbeat
      is now a smooth green breathing pattern at 1 Hz.
- [x] Capability map advertises SPI (2 instances), GPIO (1 bank),
      PWM (1 bank), ADC (1 block incl. temperature sensor) + the
      feature flags `spi.mode-0-3`, `gpio.mask-atomic`, `pwm.invert`,
      `adc.temp`.

## M4 — CDC-ACM bridge to HW UART / RS485-DE applet

### M4a — HW UART bridge (CDC #0 <-> UART0)

- [x] `src/peripherals/uart_hw.{c,h}` — IRQ-driven RX/TX ring buffers
      around the RP2350 HW UART; 2 KB RX / 1 KB TX per instance.
- [x] `src/usb/cdc_glue.{c,h}` — CDC-ACM #0 <-> UART0 HW byte shuttle
      with back-pressure on both sides; TinyUSB callbacks for line
      coding, line state (DTR/RTS), and send-break.
- [x] Capability map advertises `"uart.ttl"` and a single
      `buses.uart` instance (CDC-ACM index 0, HW backend).

### M4b — PIO UART bridge (CDC #1)

- [x] `src/pio/uart_pio_tx.pio` / `uart_pio_rx.pio` — 8N1 PIO
      programs ported from pico-examples, with RX framing-error IRQ.
- [x] `src/peripherals/uart_pio.{c,h}` — PIO-backed UART with the same
      public API as `uart_hw` (ring buffers, line-coding, tick).
      **8N1 only** — the `.pio` programs are framing-locked; non-8N1
      `SetLineCoding` requests are counted + clamped and the
      constraint is advertised as `"uart.pio.8n1-only"` and
      `framing: ["8n1"]` in the capability map. An open-config PIO
      UART (alternate programs + runtime selector) is a future item.
- [x] `cdc_glue.c` bridges CDC-ACM #1 to uart_pio instance 0
      (GP33 TX / GP36 RX on PIO1). Capability map lists the 2nd
      instance under `buses.uart`.

### M4c — UART0 phy-bank switch

- [x] `src/peripherals/uart_phy.{c,h}` — GP6/GP7/GP8 state machine with
      safe off-then-on transitions.
- [x] RPBP UART subsystem (subsys = 6): opcodes `SET_PHY (0x00)` and
      `GET_PHY (0x01)`; spec at `docs/protocol/subsystems/uart.md`.
- [x] Capability features advertise `"uart.ttl"`, `"uart.rs232"`,
      `"uart.rs485"`; each CDC is its own `buses.uart` entry with
      `phy` and an optional `de_pin` field.

### M4e — Driverless phy selection via dedicated CDC-ACMs

- [x] Composite expanded to 4 CDC-ACMs: one per UART0 phy
      (RS232 / RS485 / TTL) plus the PIO-UART. Host-facing strings
      carry the phy name so the COM ports are self-documenting.
- [x] `cdc_glue.c` watches CDC DTR transitions and auto-switches the
      UART0 phy-bank transceiver on DTR=1. Last DTR-asserted CDC wins
      mutual exclusion on UART0 HW; inactive UART0 CDCs have their
      host writes silently drained so the USB pipe does not stall.
- [x] RPBP `SET_PHY` is retained as the explicit-override path for
      terminals that do not toggle DTR and for CI scripts.
- [x] udev rules publish stable per-phy symlinks: `/dev/rpbridge-
      uart1-rs232`, `rpbridge-uart1-rs485`, `rpbridge-uart1-ttl`,
      `rpbridge-uart2`.
- [x] MS OS 2.0 descriptor set updated — vendor interface numbers
      shifted to 8 (RPBP) and 9 (gs_usb).

### M4d — RS485 DE timing

- [x] **Software DE** (current): `uart_hw` asserts GP6 HIGH before
      any TX burst and releases it once the UART BUSY flag clears,
      driven by `uart_hw_tick()` from the main loop. Correct at all
      supported baud rates (≤ 3 Mbaud) with a tick period ≤ 5 ms.
- [ ] **PIO-precision DE** (future): `src/pio/rs485_de.pio` co-
      clocked with UART0 TX, configurable guard interval. Needed
      only if a scope-level audit shows unacceptable DE edges.

## M5 — CAN over PIO (can2040)

- [x] `src/peripherals/can.{c,h}` — thin wrapper around vendored
      can2040. Claims PIO0 (3 SMs), GP20 TX / GP21 RX, 500 kbit/s
      default, bitrate reconfigurable via gs_usb BITTIMING. RX/TX
      notifications plumbed via IRQ callback → lock-free SPSC ring.
- [x] `src/usb/gs_usb_glue.{c,h}` — gs_usb (candleLight-compatible)
      protocol on vendor interface 1. Implements the classic-CAN
      subset (no FD / no timestamps) covering 100 % of SocketCAN's
      needs: HOST_FORMAT, DEVICE_CONFIG, BT_CONST, BITTIMING, MODE
      (start / reset), IDENTIFY. Echo-id FIFO for TX confirmation.
      Linux mainline `gs_usb` driver binds automatically; Windows
      via `candle_api` through our MS OS 2.0 descriptors.
- [x] Capability map advertises `buses.can` (GP20/21, backend=pio,
      transport=gs_usb) and feature flags `can.classic`, `can.gs-usb`.

## M6 — Second core + streaming

- [x] `src/io_control/io_control.{c,h}` — Core-1 scheduler entry
      (idle tick for now — anchors future ADC/logic services). Also
      hosts the cross-core event pump for Core 0 (drains GPIO edge
      events, emits RPBP events on channel 1). The directory is named
      `io_control` rather than `core1` because it describes the
      function, not the CPU.
- [x] `src/io_control/ipc_queue.h` — header-only lock-free SPSC ring
      primitive. Power-of-two slot count, fixed slot size, `__dmb()`
      barriers around head/tail publishes. Safe for IRQ → main-loop
      and Core 1 → Core 0 hand-off.
- [x] GPIO edge WATCH / UNWATCH wired through — `cmd_gpio` accepts the
      previously-reserved opcodes, IRQ installs a shared
      gpio_irq_trampoline, edges land in the io_control GPIO ring,
      main-loop pump emits RPBP EVENT frames on channel 1 with
      payload `[subsys=0x03][event=0x00][pin][level][ts_us LE u32]`.
- [ ] RPBP stream channels (dynamic channel alloc + credit accounting
      + STREAM_DATA payload framing) — scaffold only; ADC IIO and
      logic-capture producers land in M6b.

## M7 — Windows binding

- [x] `src/usb/ms_os_20.c` — BOS + MS OS 2.0 descriptor set for
      automatic WinUSB binding on Windows 10 ≥ 1709 (landed with the
      USB-stack-base change; DeviceInterfaceGUIDs are per-vendor-IF).

## M8 — Tests at scale

- [x] `tests/host/test_golden.c` — canonical-vector roundtrip corpus
      (13 test cases) covering every subsystem wire format: SYS echo/
      uptime/error, I²C probe / XFER, SPI XFER duplex, GPIO
      SET_MASK / WATCH event, PWM configure, ADC read_many, UART
      SET_PHY, 4 KB max-payload frame, RESET_CHANNEL on the events
      channel. Each frame is built + parsed + field-asserted, so
      any wire-format regression flips a single CI test red.
- [ ] `tests/host/test_sys_dispatch.c` — round-trip through
      cmd_sys + rpbp_dispatch with a stub TX capture. Blocked on
      decoupling cmd_sys.c from pico-sdk headers (time_us_64,
      bootrom, unique_id). Planned: `cmd_sys_host.c` shim or an
      `RPBP_HOST_TEST` compile flag.
- [ ] `tests/golden/*` byte-frozen corpus — hex files checked in so
      the userspace Rust decoder can consume the same vectors. M8b.
- [ ] Loopback self-test (`SELFTEST` opcode) exercising every bus
      on the live device. M8b — needs the SPI MOSI↔MISO jumper + a
      defined pass/fail bitmask format.

## M9 — Driver-optional coexistence (single configuration)

Architecture captured in
[ADR-0005](../docs/adr/0005-driver-optional-coexistence.md),
superseding the dual-configuration plan in
[ADR-0004](../docs/adr/0004-driver-priority-mode.md).

**New direction:** one USB configuration, always. 4× CDC-ACM +
2× vendor interfaces are always enumerated. The Linux driver is
strictly **additive** — it binds to the vendor interface and
spawns `auxiliary_bus` children for GPIO / I²C / SPI / PWM / ADC
(things CDC cannot express), while CDCs keep handling UART and
`gs_usb` keeps handling CAN. No `SET_CONFIGURATION` hop at driver
load, no dropped file handles, no Windows "device install"
notification storm.

### M9a — capability-map `access` tagging

- [x] ADR-0005 documents the new model. The firmware USB descriptor
      set already ships the final shape (4 CDC + 2 vendor always
      enumerated); no descriptor work needed for M9a.
- [x] Capability map gains a top-level `access` field — a 7-entry
      map `{ uart: "cdc", i2c / spi / gpio / pwm / adc / can:
      "vendor" }`. Hosts route subsystem requests to the right
      endpoint (CDC vs vendor pipe) without guessing.
- [x] `docs/protocol/capabilities.md` schema updated to include the
      `access` key — "Access map" section with value table +
      rev-B example + routing contract.

### M9b — optional UART driver claim (opt-in)

- [ ] New `SYS UART_CLAIM` / `SYS UART_RELEASE` opcodes
      (`0x09` / `0x0A`) — per-UART-instance boolean latch. While
      claimed, `cdc_glue.c` silently drains host writes to the
      matching CDC and routes bytes via RPBP stream channels
      instead. Released automatically on device disconnect.
- [ ] `cdc_glue.c`: honours the claim flag in its arbiter (one
      extra branch — cheap).
- [ ] CDC-to-UART TX path: when claimed, writes coming from a CDC
      FIFO are discarded rather than pushed to UART0 HW. RX bytes
      from UART0 HW are NOT forwarded to the CDC (host-side
      `/dev/rpbridge-uart1-*` goes dark for the duration of the
      claim).
- [ ] Ship only when the Rust driver gains a use-case that exceeds
      what termios-over-CDC already delivers. Not a blocker for
      M9a or the driver's initial release.

### M9c — kernel-driver wiring

Tracked in `driver/TODO.md`; summarised here for cross-reference:

- [ ] `rpbridge.ko` binds the vendor interface by VID/PID +
      InterfaceClass. No `SET_CONFIGURATION` at probe time.
- [ ] Walks the capability map, gates each aux child on the
      subsystem's presence.
- [ ] Spawns aux children: `gpiochip-rpbridge`,
      `i2c-adapter-rpbridge`, `spi-master-rpbridge`,
      `pwmchip-rpbridge`, `iio-rpbridge`.
- [ ] Does NOT spawn a tty driver in the first release — CDCs
      already provide `/dev/rpbridge-uart1-*` with proper termios
      via `usbser`. Optional UART claim (via M9b) is a future
      feature.
- [ ] On `disconnect()`: detach aux children cleanly; CDCs are
      handled by `usbser` independently (single-config means no
      coordination needed).

### M9d — integration tests

- [ ] umockdev recording of driver load + aux-child creation on a
      captured vendor-interface enumeration.
- [ ] KUnit: driver `probe()` spawns the right number of aux
      children for a synthesised capability map.
- [ ] End-to-end on real hardware: plug device, load driver,
      observe `/dev/i2c-N`, `/dev/spidev*`, `/dev/gpiochip*` +
      `/dev/rpbridge-uart1-*` all present simultaneously.
