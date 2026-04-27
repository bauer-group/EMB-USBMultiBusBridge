# Contributing to RPBridge

Thanks for considering a contribution. This project follows **BAUER GROUP
engineering standards**; the rules below are the ones most likely to trip
up a first-time contributor.

## TL;DR

* Fork, branch, implement, run local checks, open PR.
* Commits use **past-tense Conventional Commits**.
  * `feat(driver): added SPI controller child`
  * `fix(firmware): fixed CRC-32C on unaligned payloads`
  * **Not** `feat: add ...` (imperative is wrong here).
* Every commit must compile standalone and pass CI on its own.
* **No** `Signed-off-by` from an AI. **No** `Co-Authored-By: Claude …`
  trailers.
* All new code must ship with tests.

## Development environment

```sh
./tools/setup-dev-env.sh
```

installs, on Debian/Ubuntu:

* `gcc-arm-none-eabi`, `libnewlib-arm-none-eabi`, `cmake ≥ 3.28`, `ninja-build`
  — for the firmware.
* `linux-headers-generic` + `dkms` — for the C kernel module against the
  running kernel.
* `rustup` toolchain (`stable`) — for the userspace tree.
* `libusb-1.0-0-dev`, `libudev-dev`, `libgpiod-dev`, `libsocketcan-dev`,
  `i2c-tools`, `can-utils` — runtime + test dependencies.
* `mkdocs`, `mkdocs-material`, `mike` — for the docs site.

Re-running the script is idempotent and upgrades pinned versions where
necessary.

## Branch & PR workflow

1. Create a topic branch off `main`: `git switch -c feat/driver-spi-child`.
2. Keep commits small and focused. Squash noisy WIP commits before pushing.
3. Run local checks (see next section).
4. Push; open a PR against `main`. The CI will run the firmware build
   matrix, the driver build against all pinned kernels, the Rust unit +
   clippy checks, and the documentation build.
5. At least one approving review is required before merge (except for
   solo-developer mode on this repository; see repo settings).

## Local checks before pushing

```sh
# firmware: fast sanity build for one board
./tools/build.sh firmware --board rpbridge_rp2354b

# driver: against whichever kernel headers are installed
./tools/build.sh driver

# userspace: fmt, clippy, tests
(cd userspace && cargo fmt --all -- --check && \
                 cargo clippy --all-targets --all-features -- -D warnings && \
                 cargo test --all-features)

# docs
(cd docs && mkdocs build --strict)
```

## Coding conventions

### Rust (kernel driver + userspace)

* `cargo fmt` (rustfmt default).
* `cargo clippy -- -D warnings` — any new lint must be suppressed locally
  with a rationale in the code comment, not globally.
* Prefer `?`, iterators, and combinators over loops/early returns where
  readability is equal.
* Kernel code uses `kernel::prelude::*` only; no `std`. `no_std + alloc`.
* Userspace crates may use `std` freely. Async is `tokio` when needed.

### C (firmware)

* Indentation 4 spaces. Linux-kernel-style otherwise (braces, pointer
  declarations, naming). Functions `snake_case`. Macros `UPPER_SNAKE`.
* Headers under `firmware/src/…/*.h` carry `#pragma once`.
* No dynamic allocation on the hot path; everything on stack or in
  pre-allocated ring buffers.

### Commit messages

Past-tense subject ≤ 50 chars, no period:

```text
feat(userspace): added RPBP fragmentation path

Implemented the FRAGMENT/LAST flag handling on the client
side so frames up to 64 kB can be sent over USB-FS without
application-level chunking.

Closes #42
```

Types: `feat`, `fix`, `refactor`, `docs`, `style`, `test`, `chore`,
`perf`, `build`, `ci`, `revert`. Scopes: `firmware`, `driver`, `userspace`,
`docs`, `ci`, `hw`, `protocol`.

Breaking changes: `type!(scope): …` **or** a `BREAKING CHANGE:` footer.
Either triggers a MAJOR bump on release.

Never include AI-attribution trailers (`Co-Authored-By: Claude …` etc.)
— repo policy.

## Testing expectations

| Area                    | What is required                                           |
|-------------------------|-------------------------------------------------------------|
| Protocol changes        | Updated golden vectors in `tests/golden/` + round-trip test |
| Firmware logic          | Host-side unit test via the `firmware/tests/host` harness   |
| Driver logic            | KUnit test; umockdev replay for USB-facing behaviour        |
| Userspace libraries     | `cargo test`, property tests via `proptest` where sensible  |
| Hardware-facing changes | Bench run + note in `docs/hardware/bringup-log.md`          |

## Security & secrets

* Never commit tokens, keys, or `.env` files. `.env.example` / `.env.template`
  may be committed.
* Release signing keys live in GitHub Actions secrets, not in the tree.
* If you discover a vulnerability, see [`SECURITY.md`](SECURITY.md).

## Questions

Open a GitHub Discussion for design questions, an Issue for bugs. For
conversations that require NDA or concern an ongoing pilot customer,
contact the maintainers by email (see `SECURITY.md`).
