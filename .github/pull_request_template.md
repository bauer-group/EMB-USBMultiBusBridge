<!--
Pull-request checklist. BAUER GROUP standards apply — see
CONTRIBUTING.md and docs/commit-style.md before opening the PR.
-->

## Summary

<!-- One or two sentences. What and why. -->

## Changes

- [ ] Firmware (`firmware/`)
- [ ] Linux driver (`driver/`)
- [ ] Userspace (`userspace/`)
- [ ] Documentation (`docs/`)
- [ ] CI / packaging (`.github/`, `driver/packaging/`)
- [ ] Hardware (`hardware/`)

## Test plan

<!--
Spell out what you ran. Examples:
 - [x] `cargo test --workspace` on userspace
 - [x] `make -C driver` against kernel 6.14.0-rc3 with CONFIG_RUST=y
 - [x] cmake build of firmware for rpbridge_rp2354b, Release + RelWithDebInfo
 - [x] `mkdocs build --strict` clean
 - [x] manual: flashed `.uf2` and confirmed `i2cdetect` on /dev/i2c-N
-->

- [ ]
- [ ]

## Screenshots / logs

<!-- Only if UI or log-level changes are involved. -->

## Checklist

- [ ] Commit subject is past-tense Conventional Commits (no `Co-Authored-By` AI trailers).
- [ ] `cargo fmt --all -- --check` and `cargo clippy --all-targets --all-features -- -D warnings` pass.
- [ ] New public functions have doc comments; driver and firmware follow their tree's style.
- [ ] Breaking protocol / ABI changes bump the relevant MAJOR version and ship a migration note.
