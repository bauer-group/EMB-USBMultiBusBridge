# Golden RPBP vectors

Hand-crafted RPBP v1 frames with known-correct CRC and parse results.
Used by every language binding — firmware host tests, kernel driver,
`rpbridge-protocol` in userspace Rust, and any future third-party
implementation.

## Format

Each vector is a pair of files:

```
NNNN-<name>.bin      — raw wire bytes (header || payload || CRC)
NNNN-<name>.yaml     — metadata + expected parse result
```

`NNNN` is a zero-padded index starting at `0000`; names are
lowercase-kebab.

`NNNN-<name>.yaml` carries:

```yaml
# human-readable description
description: "CMD_REQUEST PING, empty payload, seq=42 on control channel"
# expected header fields after parse
expect:
  magic:        0x52
  version:      1
  msg_type:     PING
  flags:        []
  channel:      0
  seq:          42
  payload_len:  0
  timestamp_us: 0x12345678
# if the vector is intentionally invalid, specify why
invalid_reason: null   # or e.g. "bad_crc", "reserved_flags"
```

## Roadmap

Initial corpus (land before firmware M1):

- [ ] `0000-ping-empty.bin` — minimal PING, no payload
- [ ] `0001-pong-empty.bin` — minimal PONG
- [ ] `0002-cmd-request-cbor.bin` — CBOR-flagged CMD_REQUEST
- [ ] `0003-max-payload.bin` — maximum allowed payload, full CRC
- [ ] `0004-fragment-first.bin` / `0005-fragment-last.bin`
- [ ] `0006-bad-magic.bin` — intentionally-invalid, parser must reject
- [ ] `0007-bad-crc.bin` — intentionally-invalid
- [ ] `0008-reserved-flags.bin` — intentionally-invalid
