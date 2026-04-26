# ADR-0009 — Pin function flexibility, capability discovery and the PIN + COUNTER subsystems

**Status:** Accepted — 2026-04-26.
**Implementation milestone:** M11 (PIN subsystem) and M11b (COUNTER subsystem) — see
[`firmware/COVERAGE.md`](../../firmware/COVERAGE.md).

## Context

The product question this ADR answers is recurring:

> *"Can I just tell the device to make pin GP30 a counter / a PWM
> output / an ADC channel at runtime, and discover which functions
> a pin physically supports — through the USB protocol, without
> recompiling firmware?"*

Today, **most** of that is already possible:

- Every subsystem has a `CONFIGURE` (or implicit `claim`) path that
  takes a pin number and assigns it to the subsystem's function.
  GPIO, PWM and (lazy-claimed) SPI-CS all work this way.
- The [pin broker](../firmware/resource-budget.md#5-gpio-ownership)
  enforces single-ownership, returning `RPBP_EBUSY` when a pin is
  already taken by another subsystem.
- The capability map enumerates per-subsystem pin whitelists, so a
  host can read "PWM is available on GP30, GP31, GP37–40" without
  guessing.

But three things are missing today, all of which are exposed gaps
in the user-facing answer to the question above:

1. **No unified per-pin capability view.** The host has to
   intersect six separate `buses.<subsys>.pins` arrays to derive
   "what can GP38 actually do?". That logic is duplicated across
   userspace tools, the Rust kernel driver, and the firmware self-
   test — three places where it can drift.

2. **No atomic re-assignment.** Re-tasking a pin from PWM to GPIO
   today means: host issues "stop PWM (release)" → "configure
   GPIO". Between the two there is a window where the pin is
   tri-stated, and on multi-host setups two clients can race for
   the slot. We want one opcode that frees + claims under broker
   lock.

3. **No pulse / high-speed counter peripheral.** The RP2350 has no
   dedicated counter IP; the canonical recipe is a PIO program. We
   reserve it as a future subsystem now so the protocol surface
   doesn't need a breaking change later.

## Decision

Adopt a **two-subsystem extension** to RPBP v1, both additive (no
existing opcode renumbered, no MAJOR bump):

### PIN subsystem (`subsys = 0x0B`)

A meta-subsystem describing pins as objects rather than as members
of a per-bus whitelist. Five opcodes:

| Opcode | Name              | Purpose                                                                |
|:------:|:------------------|:-----------------------------------------------------------------------|
| `0x00` | `LIST`            | Enumerate every host-addressable pin (board-exposed).                  |
| `0x01` | `QUERY`           | Per-pin: current owner subsystem, current function tag, raw level.     |
| `0x02` | `CAPS`            | Per-pin: which subsystem-functions this pin physically supports.       |
| `0x03` | `REASSIGN`        | Atomic broker free + claim: move a pin from owner X to owner Y.        |
| `0x04` | `RESET`           | Force-release a pin, subject to a 2-byte safety token (avoids fights). |

`CAPS` is the answer to "what can GP38 do?". The reply payload is a
small bitmap encoding `IO | I²C0 | I²C1 | SPI0 | SPI1 | UART0 |
UART1 | PWM | ADC | PIO0 | PIO1 | PIO2 | COUNTER`. The host
intersects the bitmap with the current build's `RPBRIDGE_HAVE_*`
flags (already published in `features[]`) to know which functions
are reachable on this firmware.

`REASSIGN` rejects with `RPBP_EBUSY` when the destination function
is incompatible with another active claim (e.g. you can't move
GP18 from `SPI0_SCK` to `GPIO_USER` while SPI0 has a transfer in
flight). The opcode is broker-atomic: either the move succeeds in
its entirety or both sides see no state change.

Wire format details land in `docs/protocol/subsystems/pin.md` once
the implementation is on disk.

### COUNTER subsystem (`subsys = 0x0C`)

PIO-implemented edge counter — the missing peripheral the user
question called out by name. Five opcodes:

| Opcode | Name           | Purpose                                                            |
|:------:|:---------------|:-------------------------------------------------------------------|
| `0x00` | `OPEN`         | Claim a pin + edge mode (rising/falling/both) + sample window us.  |
| `0x01` | `READ`         | Snapshot the counter value (u32, optionally clear-on-read).        |
| `0x02` | `RESET`        | Zero the counter without reading it.                               |
| `0x03` | `CLOSE`        | Stop counting, return the pin to the broker.                       |
| `0x04` | `STREAM_OPEN` | Open a continuous stream emitting (window_us, count) tuples.       |

Backing implementation:

- One PIO state-machine in `PIO2` running a 6-instruction edge
  counter (program lifted from
  [pico-examples/pio/counter](https://github.com/raspberrypi/pico-examples/tree/master/pio/freq_count)
  with our naming). Worst-case: counts 75 MHz with sub-µs jitter
  on RP2350 at 150 MHz `clk_sys`.
- `peripherals/counter.{c,h}` — public API mirroring the existing
  `pwm`/`adc` modules.
- `commands/cmd_counter.c` — opcode dispatcher.
- Streaming variant rides the existing
  [stream channel layer](../protocol/rpbp-v1.md#stream-channels)
  introduced in M6b.

Resource budget impact:

- One PIO2 SM consumed (PIO2 still has 3 SMs free today — see
  [resource-budget.md §3](../firmware/resource-budget.md#3-pio-blocks-state-machines-instructions)).
- One PIO2 instruction-memory slot of ~6 instructions.
- No new DMA channel — the counter pushes one word per window via
  the SM RX FIFO; Core 0 drains in the main loop.

## Capability advertisement

Both subsystems gain entries in the capability map:

- `features[]` adds `"pin.flex"` and `"counter.pio"` so a host can
  cheaply test for the presence of either.
- `buses.pin[0]` carries the full pin enumeration with one map per
  pin: `{ gp, owner, function, caps, header }`. This is the single
  shot a host needs at attach time — subsequent operations use
  `PIN_QUERY` or `PIN_CAPS` for individual pins.
- `buses.counter[]` lists the available counter slots (one per free
  PIO2 SM). Each entry: `{ idx, max_freq_hz, modes }`.
- `access.pin = "vendor"` / `access.counter = "vendor"` per
  ADR-0005.

## Consequences

Positive:

- **Single source of truth** for per-pin capability — both the
  Rust kernel driver and userspace tooling consume `PIN_CAPS`
  instead of re-implementing the capability inference logic.
- **Atomic re-tasking** via `REASSIGN` makes pin migration a single
  RPBP exchange. Multi-client coordination becomes the broker's
  responsibility, not the host's.
- **High-speed counter** reaches the host through the standard
  RPBP / capability map / driver-aux-child pipeline — no out-of-
  band ioctl, no custom userspace daemon.
- **Forward-compatible**: existing per-subsystem `CONFIGURE`
  opcodes keep working unchanged. The PIN subsystem is a *layer
  on top of* the broker, not a replacement.

Negative:

- **Two more subsystem IDs occupied** (0x0B and 0x0C), shrinking
  the reserved range from `0x0B..0x7F` to `0x0D..0x7F`. Still 115
  IDs free — comfortable.
- **PIN_CAPS payload size**: 48 pins × 4-byte capability bitmap =
  192 bytes per "list-all" reply, well under the 4 KB MTU.
- **Driver layer growth**: the Rust kernel driver gains two thin
  wrappers (`crate::pin`, `crate::counter`). Both follow the
  three-layer pattern established in `gpio.rs` / `adc.rs` and
  reuse the FFI shims for the kernel `gpio_chip` / `counter_chip`
  bindings as those mature upstream.

## Alternatives considered

- **Stick with per-subsystem CONFIGURE only.** Today's status
  quo. Rejected — the user-visible question above remains a "you
  have to walk six different arrays" answer instead of a single
  RPBP round-trip.

- **Encode the capability bitmap directly in `buses.<subsys>.pins`
  entries.** Considered. Rejected because the capability map is
  emitted once at attach time, while the per-pin owner/function
  pair changes at runtime — a separate `QUERY` opcode is the
  right granularity.

- **Add a generic `PIN_FUNC` enum and let any subsystem CONFIGURE
  call accept it.** Considered. Rejected because each subsystem
  already has subsystem-specific configure semantics (PWM has
  freq/duty, GPIO has dir/pull). A meta-PIN subsystem keeps the
  per-subsystem opcodes cohesive while exposing the cross-cutting
  pin-as-resource view.

- **Implement the counter in software (Core 1 + GPIO IRQ).**
  Considered. Rejected — sub-µs jitter at 1 MHz+ event rates is
  not achievable without a hardware counter; PIO is the only path
  on RP2350 that meets the spec.

## Implementation sequencing

Tracked in `firmware/COVERAGE.md` under M11.

1. **M11a — PIN subsystem stub** (firmware): subsys ID + opcodes
   in `rpbp.h`, `cmd_pin.{c,h}` returning `ENOTSUP` for every op,
   capability map entry. Locks the protocol surface so userland
   tooling can build against a stable header before the body
   lands.
2. **M11b — PIN subsystem body**: `pin_broker_caps(pin)` helper
   enumerating supported functions from a compile-time table
   keyed by board header; `LIST` / `QUERY` / `CAPS` / `REASSIGN`
   / `RESET` handlers wired to the broker.
3. **M11c — COUNTER subsystem**: PIO program in
   `src/pio/edge_counter.pio`, peripheral wrapper in
   `src/peripherals/counter.{c,h}`, dispatcher in
   `src/commands/cmd_counter.c`, `OPEN` / `READ` / `RESET` /
   `CLOSE` opcodes.
4. **M11d — COUNTER streaming**: `STREAM_OPEN` opcode +
   `io_control` Core-1 service that samples the SM at the
   configured `window_us` and pushes (window, count) tuples
   through the streaming layer.
5. **Driver `crate::pin` + `crate::counter` Rust modules**:
   wire-builders + Client + auxiliary-bus children. Client +
   wire-builder layer is kernel-binding-agnostic; the
   auxiliary_bus child layer follows the gpio_chip / pwm_chip
   pattern already established.

## References

- [`docs/hardware/pin-matrix.md`](../hardware/pin-matrix.md) —
  the per-pin capability inventory this opcode set exposes
  programmatically.
- [`docs/firmware/resource-budget.md`](../firmware/resource-budget.md)
  — PIO2 has the SM headroom for the counter.
- pico-examples [`pio/freq_count`](https://github.com/raspberrypi/pico-examples/tree/master/pio/freq_count)
  — reference PIO program for the edge counter.
- ADR-0005 — `access` map informs how the host reaches each
  subsystem (PIN + COUNTER are vendor-only).
- ADR-0008 — multi-board variant pattern that PIN_CAPS reads
  the per-board pin set from.
