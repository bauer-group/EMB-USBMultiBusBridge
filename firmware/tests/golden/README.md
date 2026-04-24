# Golden RPBP wire vectors

Byte-frozen frames used to cross-validate **independent
implementations** of RPBP v1 — the firmware encoder
(`rpbp_frame_build`) and the Rust driver decoder (`rpbp::parse`).
Any implementation that produces or consumes RPBP frames MUST
agree byte-for-byte on these vectors.

## Workflow

1. A representative frame is described in the firmware-side
   roundtrip test
   [`firmware/tests/host/test_golden.c`](../host/test_golden.c)
   — e.g. "HELLO on control channel with seq=0x0042".

2. The test builds the frame with `rpbp_frame_build` and asserts a
   roundtrip through `rpbp_frame_validate`. That gives us a single
   source of truth for the byte layout on the firmware side.

3. For each frame we want to freeze, the companion generator
   `gen_golden` (not yet checked in — tracked as firmware/TODO.md
   §M8b "byte-frozen corpus") emits the raw bytes to a `.hex` file
   in this directory. Filename convention: `<msg>_<variant>.hex`,
   e.g. `hello_empty.hex`, `cmd_i2c_probe.hex`.

4. The Rust driver's own tests read the same `.hex` files and
   assert their decoder extracts matching fields. That closes the
   loop — both implementations agree on bytes AND on interpretation.

## Hex-file format

UTF-8 text. Each line contains exactly 16 bytes in `xx xx ... xx`
form, lowercase hex, no prefix. Blank lines and `#`-comment lines
are ignored. Frames larger than 16 bytes wrap across multiple
lines. Trailing newline at EOF.

Example for an empty-payload HELLO:

```text
# msg=HELLO channel=0 seq=0 payload_len=0 ts=0
52 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00
xx xx xx xx
# where xx xx xx xx is the CRC-32C of the preceding 16 header bytes.
```

The CRC is **not** pre-computed in this README because the value
changes if any header byte changes — it belongs in the `.hex` file
itself, produced by the generator.

## Why no committed vectors yet?

The generator tool depends on the host-test build (needs the
`rpbp_portable` static library). Initial corpus population is a
one-off CI step: build the tests, run `gen_golden --out
firmware/tests/golden/`, commit the output, and lock the files
against future encoder drift with a byte-exact regression check
in the Rust driver test-suite.

Until that CI step runs, this directory exists only as a contract
between the two implementations — the firmware **already**
round-trips these shapes in `test_golden.c`; the Rust side
consumes them as soon as the generator commits them.
