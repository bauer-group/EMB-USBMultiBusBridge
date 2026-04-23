# ADR-0001 — USB protocol framing

**Status:** Accepted — 2026-04-22

## Context

The device has to carry configuration, synchronous bus transactions,
unsolicited events (GPIO IRQs, bus faults), streaming data (ADC at up
to 500 kSPS, logic capture, CAN frames), and OTA control — all on a
single USB 2.0 Full-Speed link with ~1.1 MB/s of usable bulk
bandwidth.

Three realistic framings were on the table early in design:

1. **AT-style ASCII commands** on CDC-ACM, like the FTDI-FT232H /
   CH341 / MCP2221 family of bridges. Familiar, works with `screen`
   and `minicom`, but parsing is expensive, binary payloads are
   painful, and latency suffers under concurrent streams.
2. **USB HID reports.** Classless, 64-byte fixed report size. Great
   for keyboards; terrible for an adaptive 16-channel bulk protocol
   that wants frames up to 4 kB.
3. **A minimal vendor-defined bulk protocol — RPBP.** 16-byte header,
   length field, sequence for pipelining, CRC-32C for integrity,
   optional CBOR payloads for the bits that need forward-compatibility.

## Decision

Define **RPBP v1** (RPBridge Protocol) as a binary framing on a
dedicated vendor bulk endpoint. Use:

- Fixed 16-byte little-endian header.
- Trailing 4-byte CRC-32C (Castagnoli polynomial 0x1EDC6F41) for
  detecting firmware bugs and ESD bit flips, independent of the USB
  transport CRC.
- Channel multiplexing inside the single bulk pipe for parallel
  control / stream / event paths, rather than a separate USB
  interface per subsystem.
- Credit-based flow control so one saturated stream cannot starve the
  others.
- CBOR (RFC 8949) for capability maps and extensible command payloads
  — no schema compiler, small library footprint.

CAN uses a **separate** `gs_usb`-compatible vendor interface so that
mainline `drivers/net/can/usb/gs_usb.ko` binds it automatically. UART
traffic is carried over three standard CDC-ACM interfaces. RPBP only
covers the remaining subsystems (I²C, SPI, GPIO, PWM, ADC, UART phy
selection, events, OTA triggers).

See [`docs/drafts/rpbridge-driver-stack.md`](../drafts/rpbridge-driver-stack.md) §4
for the full wire-format proposal this ADR accepts.

## Consequences

Positive:

- Sub-millisecond round-trip for short commands, sustained ~900 kB/s
  on bulk streams — within USB-FS ceiling.
- Binary-structured payloads for hot paths (`memcpy` on the MCU),
  CBOR only where forward-compat is valued.
- One implementation of the framing: `rpbridge-protocol` Rust crate,
  mirrored by `firmware/src/rpbp/` in C and `driver/src/rpbp.rs` in
  kernel Rust. Golden vectors under `tests/golden/` keep them in sync.

Negative:

- We give up "plug it into `screen`" discoverability for the
  non-UART subsystems. Mitigated by `rpbridge-ctl` in userspace.
- Every language binding has to carry an RPBP framer. Acceptable —
  the framer is ~300 LoC.

## Supersedes

Supersedes §5.3 ("Command-Framing") of `docs/drafts/rpbridge-architektur.md`
which left the framing deliberately vague.
