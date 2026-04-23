# Commit style

All commits in this repository follow **past-tense Conventional
Commits** per the BAUER GROUP standard. Examples:

```text
feat(driver): added SPI controller child
fix(firmware): fixed CRC-32C on unaligned payloads
docs(protocol): clarified fragment flag ordering
refactor(userspace): split framing from transport in rpbridge-protocol
```

## Rules

- **Subject** ≤ 50 characters, **past tense**, no trailing period.
- Use the vocabulary: `added`, `fixed`, `updated`, `removed`,
  `refactored`, `improved`, `implemented`. Not `add`, not `fix`.
- **Types**: `feat`, `fix`, `refactor`, `docs`, `style`, `test`,
  `chore`, `perf`, `build`, `ci`, `revert`.
- **Scopes** in this repository: `firmware`, `driver`, `userspace`,
  `protocol`, `docs`, `ci`, `hw`.
- **Breaking change**: use `type!(scope): …` or a `BREAKING CHANGE:`
  footer. Either triggers a MAJOR bump on the next release.
- **Atomic**: one logical change per commit. Each commit must build
  and pass tests standalone.
- **No AI trailers** — `Co-Authored-By: Claude …` and similar lines
  are forbidden by repo policy.

## Body and footer

Optional body describes **what changed and why**. Wrap at 72 columns.
Footers contain machine-readable metadata:

```text
feat(userspace): added fragmentation path

Implemented the FRAGMENT / LAST flag handling on the client side so
messages larger than the negotiated MTU can be sent over USB-FS without
application-level chunking.

Closes #42
```

## Automated checks

- `commitlint` is wired into the CI (TODO) and will reject imperative
  subjects.
- `cargo clippy`, `cargo fmt`, `checkpatch`-style checks for the driver,
  and `arm-none-eabi-size` for the firmware all need to be green on
  every commit before it is pushed to `main`.

## Solo-developer mode

The repo is under solo-developer-mode by default: direct pushes to
`main` are allowed. A PR workflow is activated only when the
maintainer explicitly asks for one.
