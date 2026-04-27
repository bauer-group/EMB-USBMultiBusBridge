# Firmware — implementation roadmap (working backlog)

> **Looking for the at-a-glance status report?** See
> [`COVERAGE.md`](COVERAGE.md) — the consumer-facing "what's
> implemented + tested" matrix, kept in lock-step with this file
> and updated whenever a milestone closes.
>
> This page is the **developer working backlog**: granular
> per-component checkboxes, in-flight items, and post-1.0 follow-
> ups. New tasks land here; closed milestones graduate into the
> coverage matrix.

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
- [x] `src/rpbp/rpbp_stream.{c,h}` — dynamic channel registry
      (up to 4 simultaneous streams, each tracked in a slot with
      credit counter + TX seq + drop stats). `rpbp_stream_open(ch)`
      / `rpbp_stream_close(ch)` / `rpbp_stream_send_frame(ch, …)`.
      STREAM_CREDIT dispatcher hooks `rpbp_stream_on_credit()` into
      the registry so host-side credit refills land directly.
- [x] ADC IIO-streamer on Core 1 — `io_adc_sample` SPSC ring + a
      drift-free 1 µs-aligned sampler in `io_control`'s core1_entry.
      Core 0's `io_control_poll()` drains the ring + emits
      STREAM_DATA frames on the active stream channel.
- [x] `ADC_STREAM_START` (0x04) / `ADC_STREAM_STOP` (0x05) opcodes
      in `cmd_adc.c`; rate clamped to 1..10 000 Hz (USB-FS bandwidth
      ceiling). Payload format documented in
      `docs/protocol/subsystems/adc.md` #STREAM_START.
- [x] Capability-map feature flag `"adc.stream"`.

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
- [x] `tests/host/test_sys_dispatch.c` — round-trip through
      `rpbp_dispatch_frame` with a `capture_tx()` TX sink. 7 cases:
      HELLO-opens-session, PING→PONG, CMD_REQUEST-pre-HELLO
      returns EPERM, CMD_REQUEST routing to subsystem stub,
      RESET_CHANNEL ack, wrong-direction ERROR, unknown-msg_type
      rejection. Decoupling handled via a new
      `tests/host/host_stubs.c` shim that stands in for pico-sdk
      `time_us_64()` + all 7 `cmd_*_dispatch()` handlers.
- [x] `tests/golden/` corpus scaffold — `gen_golden` utility emits
      7 canonical wire vectors (HELLO empty, PING ctrl-seq42, SYS
      ECHO 'Hi!', I2C PROBE, GPIO EDGE event, STREAM_DATA ADC,
      RESET_CHANNEL events) as `.hex` files. README.md describes
      the format + cross-impl contract. Initial file population is
      a one-off CI step (`cmake --build . --target gen-golden`);
      after that the files are checked in and become the byte-
      frozen regression fence for both the C encoder and the Rust
      decoder.
- [x] Loopback self-test (`SELFTEST` opcode) exercises every bus
      on the live device. 10 tests covered: pin_broker integrity,
      CRC-32C known-vector (RFC 3720), clk_sys range, ADC temp
      sensor sensibility, I²C #0 baudrate set, SPI #0 SET_FREQ
      round-trip, PWM-driven LED running, CAN bitrate non-zero,
      Core 1 liveness counter advancing, watchdog CTRL.ENABLE set.
      Pass/fail encoded as a u32 bitmap; full spec in
      `docs/protocol/subsystems/sys.md` #0x06. Tests that would
      need an external jumper (SPI MOSI↔MISO loopback, UART TX↔RX)
      remain as future extensions on unused bits.

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

- [x] New `SYS UART_CLAIM` / `SYS UART_RELEASE` opcodes
      (`0x09` / `0x0A`) — per-UART-instance boolean latch via a
      2-bit `g_uart_claim_mask` in cmd_sys.c. Idempotent claim /
      release; invalid `uart_idx` returns `EINVAL`; claims cleared
      on device reset (no persistence).
- [x] `cdc_glue.c` honours the claim flag in its arbiter — two
      new branches (one per UART instance) that swap the bidirec-
      tional pump for `drain_inactive_cdc()` when claimed.
- [x] CDC-to-UART TX path: when claimed, writes from the CDC FIFO
      are silently drained (via `drain_inactive_cdc`) and UART RX
      is NOT forwarded to the CDC. Host-side
      `/dev/rpbridge-uart1-*` goes dark for the duration of the
      claim; UART HW keeps running so the claim holder can read via
      `uart_hw_read()` / `uart_pio_read()` through its own path.
- [x] Documented in `docs/protocol/subsystems/sys.md` #0x09 /
      #0x0A. Reserved-opcodes range tightened to `0x0B..0x7F`.

### M9c — kernel-driver wiring

Tracked in `driver/TODO.md`; summarised here for cross-reference:

- [x] `rpbridge.ko` binds the vendor interface by VID/PID +
      InterfaceClass. No `SET_CONFIGURATION` at probe time.
      *(Structure in place; URB/endpoint code gated on R4L-USB
      bindings — see `driver/TODO.md` #M10c.)*
- [x] Walks the capability map, gates each aux child on the
      subsystem's presence — `spawn_children()` in core_driver.rs.
- [x] Spawns aux children: `rpbridge-gpio`, `rpbridge-i2c`,
      `rpbridge-spi`, `rpbridge-pwm`, `rpbridge-iio`. UART and CAN
      are skipped by design (ADR-0006).
- [x] Does NOT spawn a tty driver in the first release — CDCs
      already provide `/dev/rpbridge-uart1-*` with proper termios
      via `usbser`. Optional UART claim (via M9b) retained as a
      diagnostic escape hatch only.
- [ ] **Blocked on R4L-USB-BINDINGS:** `disconnect()` calls
      `usb_kill_anchored_urbs` + auxiliary_device_unregister for
      each child.

### M9d — integration tests

- [x] umockdev recording template + replay script — see
      `driver/tests/umockdev/`.
- [x] KUnit scaffold: CRC-32C parity + golden-vector CRC trailer
      agreement — see `driver/tests/kunit/`.
- [x] End-to-end hardware checklist with 10 sections covering
      every ADR claim — see `driver/tests/e2e.md`.

## M10 — Driver-native RPBP client + gs_usb feature completion (ADR-0006)

### M10a — gs_usb HW_TIMESTAMP

- [x] `GS_CAN_FEATURE_HW_TIMESTAMP` advertised in
      `gs_device_bt_const.feature` (gs_usb_glue.c).
- [x] `gs_host_frame` carries a trailing LE u32 µs timestamp when
      the host opts in via `GS_CAN_MODE_HW_TIMESTAMP`. Wire size
      selector `host_frame_size()` returns 20 / 24 symmetrically.
- [x] `GS_USB_BREQ_TIMESTAMP` IN request returns the current
      device µs counter for host clock anchoring.
- [x] Capability flag `"can.hw-timestamp"` in the features array.

### M10b — CAN-FD (hardware-gated)

- [x] Documented as a hardware gate — see
      `docs/protocol/hardware-canfd.md`. can2040 is Classic-only;
      FD needs external SPI CAN-FD silicon (MCP2518FD) or new
      silicon with integrated FD core.
- [x] `RPBRIDGE_HAVE_CANFD` compile-time gate in gs_usb_glue.c
      hard-fails the build if enabled without an FD-capable
      backend.

### M10 driver-side

See `driver/TODO.md` for the client/adapter completion state
(M10c..M10i).

## M11 — Multi-board support (ADR-0008)

Refactored every hardcoded GP-pin and per-subsystem instance count
into board-header macros so additional variants land as
"new header file + CMake matrix entry" rather than source edits.

- [x] **M11.1** — extended `boards/rpbridge_rp2354b.h` with the full
      peripheral pin map + feature flags (`RPBRIDGE_HAVE_*`) for the
      flagship rev-B board. No source change; defaults preserve
      existing behaviour exactly.
- [x] **M11.2** — `pin_broker.h` `PIN_BROKER_NUM_PINS` and `adc.{h,c}`
      channel map driven by board macros.
- [x] **M11.3** — `gpio.{h,c}` and `pwm.{h,c}` user whitelists driven
      by `RPBRIDGE_USER_GPIO/PWM_PIN_LIST`.
- [x] **M11.4** — `i2c_hw.{h,c}` and `spi.{h,c}` use board pin macros
      and gate bus #1 on `RPBRIDGE_HAVE_I2C1` / `RPBRIDGE_HAVE_SPI1`.
- [x] **M11.5** — `uart_phy.c` and `cdc_glue.c` migrated to board
      macros; phy bank gated on `RPBRIDGE_HAVE_RS232_PHY` /
      `RPBRIDGE_HAVE_RS485_PHY`; PIO-UART gated on
      `RPBRIDGE_HAVE_PIO_UART`.
- [x] **M11.6** — `capabilities.c` is fully board-flag driven: every
      array length and per-bus entry is computed at compile time
      from `RPBRIDGE_HAVE_*`. Single source of truth for "what does
      this build expose".
- [x] **M11.7** — `boards/rpbridge_pico2.h` added (Pi Pico 2,
      RP2350A QFN-60). I²C #1 + SPI #1 absent, no transceivers,
      PIO-UART remapped to GP6/GP7, user GPIO/PWM on GP8-15.
- [x] **M11.8** — `CMakeLists.txt` validates `PICO_BOARD` against
      `RPBRIDGE_SUPPORTED_BOARDS`, output binary renamed per variant
      (`rpbridge-rev-b.uf2`, `rpbridge-pico2.uf2`).
      `firmware/build-all-variants.sh` builds every variant in one
      shell loop. `.github/workflows/firmware.yml` matrix axis
      `board` includes both variants → 4-job matrix per push.

### M11 follow-ups (non-blocking)

- [ ] **CDC count per variant** — TUSB `CFG_TUD_CDC` is a compile
      constant; bare-board variants currently enumerate 4 CDCs
      where only TTL is meaningful. A variant-driven CDC count
      collapses to 1 CDC on Pi Pico 2.
- [x] **Single-LED gate** — `RPBRIDGE_HAVE_RGB_LED` flag selects
      between three-channel PWM RGB (rev-B) and a single-channel
      max(r,g,b) brightness path (Pico 2). `main.c::led_init` and
      `led_apply_rgb` branch on it.
- [x] **CI `grep` check** — `firmware/scripts/check-pin-discipline.sh`
      audits 22 pin-typed pico-sdk functions in `firmware/src/`,
      fails on hardcoded GP-literals that should go through a
      `RPBRIDGE_*_PIN` macro. Wired into
      `firmware/build-all-variants.sh` so every local + CI build
      runs the gate. Inline opt-out: `// pin-discipline: ok`.

## M12 — PIO2 applets: WS2812 + DMX512

PIO2 was unused before this milestone. Two applets land on it
together so the block budget is set in one go:

- [x] `src/pio/ws2812.pio` — 4-instr NRZ encoder (T1/T2/T3 split,
      side-set every instruction). SM clock 8 MHz → 1.25 µs/bit.
- [x] `src/peripherals/led_strip.{c,h}` — driver on PIO2 SM 0.
      4 B/pixel framebuffer (max 256 px = 1 KB), GRB / RGB / GRBW
      format selector, 80 µs latch hold. Pin = board-defined
      `RPBRIDGE_LED_STRIP_PIN`.
- [x] `src/commands/cmd_led.{c,h}` — RPBP subsystem `0x07` opcodes
      `GET_INFO (0x00)`, `CONFIGURE (0x01)`, `WRITE (0x02)`,
      `CLEAR (0x03)`. Spec at `docs/protocol/subsystems/led.md`.
- [x] `src/pio/dmx_tx.pio` — 8N2 UART at 250 kbaud (~5 instr).
      BREAK + MAB are generated by the C driver via
      `pio_sm_set_pins_with_mask` while the SM is paused — keeps
      BREAK timing host-tunable.
- [x] `src/peripherals/dmx.{c,h}` — driver on PIO2 SM 1. 513-byte
      universe buffer (start code + 512 channels), tunable BREAK /
      MAB (≥ spec minima), `dmx_tick()` runs the ~40 Hz frame
      cadence from the main loop. Pin = `RPBRIDGE_DMX_TX_PIN`
      (user wires their own RS-485 transceiver).
- [x] `src/commands/cmd_dmx.{c,h}` — RPBP subsystem `0x08` opcodes
      `GET_INFO (0x00)`, `SET_SLOTS (0x01)`, `START (0x02)`,
      `STOP (0x03)`, `SET_BREAK (0x04)`. Spec at
      `docs/protocol/subsystems/dmx.md`.
- [x] Capability map advertises `buses.led` + `buses.dmx`, feature
      flags `led.ws2812` + `dmx.tx`, access map entries
      `led: vendor` + `dmx: vendor`.
- [x] Board macros: `RPBRIDGE_HAVE_LED`, `RPBRIDGE_HAVE_DMX`,
      `RPBRIDGE_LED_STRIP_PIN`, `RPBRIDGE_DMX_TX_PIN`,
      `RPBRIDGE_LED_MAX_PIXELS`. Defaulted on for both
      `rpbridge_pico2` (GP14 / GP15) and `rpbridge_rp2354b`
      (GP43 / GP44).

### M12 follow-ups (non-blocking)

- [ ] **SK6812-RGBW PIO program** — 32 bits/pixel so the W byte is
      actually transmitted. Today GRBW format is framebuffer-aware
      but the SM only shifts the top 24 bits.
- [ ] **DMX RX** — for RDM (DMX remote-device management) and
      receiver use cases. Adds a second SM on PIO2.
- [ ] **DMX phy reuse** — let DMX share the on-board UART0 SN65HVD72
      RS-485 transceiver via a `UART_CLAIM`-style mux instead of
      requiring an external transceiver on the user header pin.

### M12.2 — 1-Wire master + I²S TX (PIO1 + PIO2 expansion)

Two more PIO applets to maximise the chip's programmable-IO budget.
PIO0 stays sealed for can2040; PIO1 and PIO2 each pick up one extra
SM, leaving 1 SM and 6-15 instr free in each block for the planned
logic-capture / RS485 precision-DE applets.

- [x] `src/pio/onewire.pio` — Maxim/Dallas master with
      pindirs-as-open-drain emulation. ~14 instr. Emits one bit-time
      slot per FIFO command (WRITE_0, WRITE_1, RESET); samples bus
      and pushes the read bit back via the RX FIFO.
- [x] `src/peripherals/onewire.{c,h}` — driver on PIO1 SM 2.
      Composes 8-bit bytes (LSB-first per AN126), exposes
      `reset_present`, `read_byte`, `write_byte`, `triplet`
      (SEARCH-ROM primitive). Pin = `RPBRIDGE_ONEWIRE_PIN`.
- [x] `src/commands/cmd_onewire.{c,h}` — RPBP subsystem `0x09`
      opcodes `GET_INFO (0x00)`, `RESET (0x01)`, `WRITE (0x02)`,
      `READ (0x03)`, `TRIPLET (0x04)`. Spec at
      `docs/protocol/subsystems/onewire.md`.
- [x] `src/pio/i2s_tx.pio` — Philips I²S TX with side-set 2
      (BCLK + LRCLK). 8 instr, MSB-first 16-bit stereo, SM clock =
      64 × sample_rate. Mirrors pico-extras `audio_i2s.pio` (BSD-3).
- [x] `src/peripherals/i2s.{c,h}` — driver on PIO2 SM 2. Static
      `_Static_assert` enforces `LRCLK_PIN == BCLK_PIN + 1` for
      side-set. Sample rate 8 kHz..96 kHz; 16-bit stereo PCM
      packed L_le16 + R_le16 per frame. v1 is buffer-playback
      (WRITE blocks until FIFO accepts), continuous streaming via
      RPBP STREAM channel is a follow-up.
- [x] `src/commands/cmd_i2s.{c,h}` — RPBP subsystem `0x0A`
      opcodes `GET_INFO (0x00)`, `CONFIGURE (0x01)`,
      `START (0x02)`, `STOP (0x03)`, `WRITE (0x04)`. Spec at
      `docs/protocol/subsystems/i2s.md`.
- [x] Capability map advertises `buses.onewire` + `buses.i2s`,
      feature flags `onewire.master` + `i2s.tx`, access-map
      entries `onewire: vendor` + `i2s: vendor`.
- [x] Board macros: `RPBRIDGE_HAVE_ONEWIRE`, `RPBRIDGE_HAVE_I2S`,
      `RPBRIDGE_ONEWIRE_PIN`, `RPBRIDGE_I2S_BCLK_PIN` /
      `_LRCLK_PIN` / `_DOUT_PIN`. Default-on for both shipping
      variants. rev-B: GP32 / GP41 / GP42 / GP45.
      pico2: GP12 / GP8 / GP9 / GP13.
- [ ] Optional follow-up: **PIO bring-up validation** — both
      programs are written conservatively (citing pico-examples and
      Maxim AN126), but only the WS2812 program has a validated
      reference build. The 1-Wire bit-slot timings and the I²S
      side-set placement need scope-level validation against an
      actual DS18B20 and PCM5102A respectively. Fold into the M3
      bring-up checklist in `docs/firmware/resource-budget.md`.
- [ ] Optional follow-up: **continuous I²S streaming** via an RPBP
      STREAM_DATA channel on a dynamic channel ID. Today the host
      must call WRITE every ~10 ms to keep the FIFO fed at 48 kHz;
      the STREAM model would let it queue in larger units with
      credit flow control like the ADC streamer.
- [ ] Optional follow-up: **1-Wire overdrive speed** (~10 µs slots)
      for slaves that support it (DS28E10, DS28EC20). The current
      PIO clkdiv yields 1 µs/cycle; overdrive needs 0.1 µs/cycle
      (15 MHz SM) which is within the RP2350 PIO ceiling at default
      150 MHz clk_sys.

## M13 — I²S continuous streaming (host → firmware)

Adds a continuous PCM streaming path on top of the existing I²S
subsystem so userspace can pipe an arbitrarily-long audio source
through the bridge (audio playback, beacon tones, latency-bounded
TX of pre-recorded streams). Today I²S is request/response —
`I2S_WRITE` writes a one-shot buffer per RPBP call.

### Architectural premise

The existing `rpbp_stream` module is **producer→host** only (ADC
streamer, future logic-analyser): firmware emits `STREAM_DATA`
frames, host advertises credit. M13 is the **inverse** direction —
host emits `STREAM_DATA` frames, firmware consumes them into an
I²S TX DMA ring buffer and advertises credit back as the ring
empties. The two directions share the channel-id range and the
RPBP `STREAM_DATA` / `STREAM_CREDIT` message types but live in
separate registries because the per-slot state is opposite.

### Sub-tasks

- [ ] **rpbp_stream_rx.{c,h}** — sibling module to `rpbp_stream`.
      Per-channel ring buffer + DMA bind hook + credit emitter.
      API surface:
        `int  rpbp_stream_rx_open(uint16_t channel, void *ring,
                                  size_t ring_bytes,
                                  void (*on_data_avail)(void *),
                                  void *cb_arg);`
        `int  rpbp_stream_rx_close(uint16_t channel);`
        `void rpbp_stream_rx_on_data(uint16_t channel,
                                     const uint8_t *frame,
                                     size_t frame_len);`
        `void rpbp_stream_rx_advance(uint16_t channel,
                                      size_t bytes_consumed);`
      The dispatcher routes `RPBP_MT_STREAM_DATA` frames whose
      channel is in the rx-registry to `rpbp_stream_rx_on_data`;
      DMA completion ISR calls `rpbp_stream_rx_advance` to free
      ring space, which triggers a `STREAM_CREDIT` emission to
      the host.

- [ ] **`firmware/src/peripherals/i2s.{c,h}` DMA chaining** —
      adapt the per-call WRITE DMA channel into a chained
      configuration that drains a ring buffer continuously.
      Underrun handling: when the ring is empty and DMA
      completes, emit silence (zero-fill) and flag a sticky
      `underrun_count` for the next `GET_INFO` reply.

- [ ] **`firmware/src/commands/cmd_i2s.c` opcodes** — extend the
      I²S subsystem (0x0A):
        `I2S_STREAM_OPEN  (0x06)` — args: `bus_idx, ring_bytes`
                                    returns: assigned channel id +
                                    initial credit (= ring_bytes).
        `I2S_STREAM_CLOSE (0x07)` — args: `bus_idx`.
      Existing `I2S_WRITE (0x05)` stays for one-shot use.

- [ ] **Userspace `userspace/rpbridge-protocol/src/subsys/i2s.rs`**
      — request builders and response parsers for the two new
      opcodes; STREAM_DATA framing helper for callers that prefer
      not to touch RPBP transport directly.

- [ ] **Userspace `userspace/rpbridge-ctl/`** — `i2s stream`
      subcommand that pipes stdin (raw 16-bit stereo PCM at the
      configured sample rate) to the firmware via the new
      streaming channel. Mirrors the `adc stream` shape.

- [ ] **Host-side test in `firmware/tests/host/`** — ring-buffer
      append + advance + credit-emission unit test. Underrun
      path test (empty-ring DMA completion).

- [ ] **Hardware validation** — pipe a sine-wave WAV file
      (`ffmpeg -i input.wav -f s16le -ar 48000 -ac 2 - | rpbridge-ctl
      i2s stream`) to a real I²S DAC, confirm continuous output
      without audible artefacts. Listening test, oscilloscope
      not strictly needed if no underruns and the credit log is
      clean.

### Out of scope (deferred follow-ups)

- I²S RX continuous streaming. The chip can do it, but no real
  use-case has materialised; if/when it does, the inverse path
  reuses the existing `rpbp_stream` (firmware→host).
- Sample-rate conversion in firmware. Userspace converts before
  feeding the stream.
- Format negotiation beyond 16-bit stereo. Float / 24-bit are
  imaginable but each adds DMA-burst complexity; not blocking.

### Effort

~600 LoC across firmware + userspace; 2–3 dev-days including
hardware validation against a real I²S DAC.

## Pre-production gates

Things that must be checked off before any public hardware shipment.
Tracking lives here so we don't ship a board with a placeholder.

- [x] **USB VID/PID** — production allocation `0545:0101` (BAUER
      GROUP / RPBridge) wired in firmware (`usb_descriptors.c`),
      driver (`driver/src/rpbp.h`), userspace (`rpbridge/src/lib.rs`),
      and udev rules on 2026-04-27. See
      [`docs/usb-vid-pid.md`](../docs/usb-vid-pid.md) for the
      lock-step list and interface-class layout.
- [x] **bcdDevice** — closed without a bump. The convention is
      "couple to RPBP wire-protocol MAJOR version" (RPBP v1 →
      `0x0100`), not "bump per production-ID release". Coupling
      to firmware semver would force Windows (which keys on
      `REV_xxxx`) to re-install drivers + re-pair applications
      on every patch release. The constant now lives as
      `RPBRIDGE_BCD_DEVICE` in `firmware/src/usb/usb_descriptors.c`
      with the rationale inline. Next bump: when RPBP rolls to v2.
- [ ] **udev rules + umockdev recording** — udev rules already
      match the production VID `0545:0101`. The umockdev fixture
      is the only piece that still carries pre-production-VID
      traffic. Refresh requires a recording session against real
      hardware (see
      [`driver/tests/umockdev/README.md`](../driver/tests/umockdev/README.md));
      can't be done at the desk. Bench-blocked.
