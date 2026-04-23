# Firmware host-side unit tests

Cross-compile-free tests for the portable RPBP modules
(`rpbp_framing`, `rpbp_crc32c`, `rpbp_cbor`). They run on the host
compiler (gcc / clang / MSVC) without pico-sdk and without flashing
hardware — ideal for fast iteration and CI.

## Build and run

```sh
cmake -S firmware/tests/host -B firmware/tests/host/build
cmake --build firmware/tests/host/build --parallel
ctest --test-dir firmware/tests/host/build --output-on-failure
```

Expected output on a passing run:

```text
Test project .../firmware/tests/host/build
    Start 1: test_crc32c
1/3 Test #1: test_crc32c ......................   Passed
    Start 2: test_framing
2/3 Test #2: test_framing .....................   Passed
    Start 3: test_cbor
3/3 Test #3: test_cbor ........................   Passed

100% tests passed, 0 tests failed out of 3
```

## What is covered

| Test suite        | Source             | Verified behaviour                                                     |
|-------------------|--------------------|------------------------------------------------------------------------|
| `test_crc32c`     | `rpbp_crc32c.c`    | Empty string, RFC 3720 reference vector, single-byte vector, streaming vs one-shot equivalence, fixed 64-byte vector. |
| `test_framing`    | `rpbp_framing.c`   | Header encode/decode roundtrip, bad magic / version / reserved flags / fragment+last / oversize payload rejections, frame build+validate, CRC bit-flip detection, length-mismatch detection. |
| `test_cbor`       | `rpbp_cbor.c`      | RFC 8949 Appendix-A vectors for uint/text/array/map, empty byte string, bool/null simple values, sticky-error on overflow. |

## What is NOT covered here

- `rpbp_transport.c` and `rpbp_dispatch.c` pull in pico-sdk (time,
  TinyUSB). They are exercised against real hardware or via
  `umockdev` replay, not on the host-test track.
- Golden-vector corpus replay — lives under `tests/golden/` at the
  repository root and is consumed by both the firmware tests and the
  userspace Rust `rpbridge-protocol` crate (once the vectors are
  committed; see `tests/golden/README.md`).

## Adding a test

1. Add a `test_*.c` file under this directory, include `test_util.h`,
   and define one or more `TEST_CASE(...)` functions plus a `main()`
   that calls `RUN(...)`.
2. Append the test name to the `foreach()` list in
   [`CMakeLists.txt`](CMakeLists.txt).
3. Reconfigure + rebuild + rerun CTest.

Failure output includes the file + line of the first failed
assertion; the harness exits with status 1 on the first failure in a
suite (early-abort behaviour keeps the signal:noise ratio high).
