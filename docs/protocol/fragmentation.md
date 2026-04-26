# RPBP v1 — fragmentation

Detailed rules and worked examples for splitting RPBP messages larger
than `MAX_PAYLOAD_LEN = 4096` bytes. Normative; see
[`rpbp-v1.md` §7](rpbp-v1.md#7-fragmentation) for the brief summary.

## Why fragmentation exists

USB 2.0 Full-Speed caps single-transfer size in practice at a few
kilobytes, and RPBP caps `payload_len` per frame at 4 kB to keep
memory planning on the MCU predictable. The capability map is the
main consumer — devices with many advertised buses have maps that
exceed 4 kB — but any large CBOR response benefits.

## Flag encoding

Three flag bits are used:

| `FRAGMENT` | `LAST` | `CONTINUATION` | Meaning                     |
|:----------:|:------:|:--------------:|------------------------------|
| 0          | 0      | 0              | Complete single-frame message. |
| 1          | 0      | 0              | First fragment.               |
| 1          | 0      | 1              | Intermediate fragment (alias for `FRAGMENT=1, LAST=0`). Receivers MUST accept either encoding. |
| 0          | 1      | 0              | Last (terminating) fragment. |
| 1          | 1      | any            | **Invalid**. Receivers MUST reject with `EPROTO`. |

Senders SHOULD emit `FRAGMENT=1` without `CONTINUATION` on
intermediate fragments; `CONTINUATION` is retained for readability
and future-proofing.

## Sequence rules

Within a single fragmented message on one channel:

- Sequence numbers MUST be **strictly monotonic** with **no gaps**.
  A receiver tracks `expected_next_seq`; if an arriving fragment has
  any other seq, it responds `ERROR { status = EPROTO }` and discards
  the partial message.
- The first fragment's seq MAY be any value the sender chooses; all
  subsequent fragments follow sequentially.
- Sequence numbers wrap at 65536. A fragmented message MAY span the
  wrap; the receiver treats `seq = 0` after `seq = 65535` as
  continuous.

## Interleaving ban

On a given channel, one multi-fragment message MUST complete before
another begins. Interleaving is forbidden.

Rationale: without per-message IDs, interleaved fragments are not
disambiguable. If concurrent transmission is required, use multiple
channels.

## Reassembly

- The reassembled payload is the **byte concatenation** of the
  per-fragment payloads, in seq order.
- Fragment count is not carried in the protocol; the receiver stops
  reassembly when it sees a frame with `LAST=1` (or a complete
  single-frame message with neither `FRAGMENT` nor `LAST`).
- The reassembled message inherits the `msg_type`, `flags` (minus
  FRAGMENT/LAST/CONTINUATION), `channel` of the **first** fragment.
  Other header fields (`seq`, `payload_len`, `timestamp_us`, CRC) are
  per-frame.
- Receivers MUST verify the CRC on every fragment. A CRC failure on
  any fragment discards the in-progress message and causes an
  `ERROR { status = ECRC, orig_channel, orig_seq = <failed frame> }`
  reply.

## Maximum reassembled message size

- The v1 minimum conformance level is **64 kB** of reassembled
  payload.
- Implementations MAY advertise a higher ceiling via the
  `"max_reassembled"` capability feature (byte value).
- Senders MUST NOT emit reassembled payloads larger than the smallest
  announced ceiling among peers. Over-large payloads are rejected
  with `ERROR { status = EMSGSIZE }`.

## Reset, cancellation, teardown

Any of the following discards the in-progress reassembly state on the
affected channel:

- `RESET_CHANNEL` message.
- USB interface disconnect / reconnect.
- CRC failure on any fragment of the current message.
- Receipt of a frame with an unexpected `seq`.

After a discard, the next fragment received on that channel MUST be
the first fragment of a **new** message (either non-fragmented, or a
first fragment with `FRAGMENT=1, LAST=0`). A mid-sequence fragment
after a discard yields `ERROR { status = EPROTO }`.

## Worked example — 10 000-byte capability map

Device sends a 10 000-byte CAPABILITIES payload in 3 fragments:

| Fragment | seq   | `payload_len` | `FRAGMENT` | `LAST` | `CONTINUATION` | Payload bytes |
|----------|------:|--------------:|:----------:|:------:|:--------------:|--------------:|
| 1 / 3    | 10    | 4096          | 1          | 0      | 0              | 0..4095       |
| 2 / 3    | 11    | 4096          | 1          | 0      | 0 (or 1)       | 4096..8191    |
| 3 / 3    | 12    | 1808          | 0          | 1      | 0              | 8192..9999    |

On receipt of frame 12, the host concatenates payload-byte-streams
(0..4095) + (4096..8191) + (8192..9999) = 10 000 bytes, then runs
the CBOR parser over the result.

## Worked example — error recovery

Device starts a 3-fragment response:

- seq 42 (first, FRAGMENT) arrives OK.
- seq 43 (intermediate) arrives with a bit flip → CRC fails on the host.

Host:

1. Discards reassembly state for this message.
2. Sends `ERROR { status = ECRC, orig_channel = 0, orig_seq = 43 }`.

Device:

1. Receives the ERROR, logs it.
2. MAY retry the whole response from a new seq (e.g. seq 50), or
   abandon the operation and reply `CMD_RESPONSE { status = EIO }`
   on the original request — implementation-defined.

Either way, there is no "resume from fragment 2" mechanism: the unit
of retry is the whole message.

## Non-goals

- **Out-of-order reassembly.** Not supported. USB bulk is in-order.
- **Per-fragment retransmit.** Not supported. USB provides transport
  reliability; if an entire frame is lost, the upper layer retries.
- **Cross-channel fragmentation.** A message is bound to a single
  channel for the duration of its transmission.

## Test vectors

Golden vectors covering fragmentation live at
[`tests/golden/0004-fragment-first.bin`](https://github.com/bauer-group/EMB-USBMultiBusBridge/tree/main/tests/golden)
through `0007-fragment-*`. See the corpus README for naming conventions
and expected metadata.
