# Releases

Releases are tagged `vX.Y.Z` on `main`. Each tag bundles
**compatible** versions of the four independently-versioned
components:

- RPBP protocol spec
- Firmware build
- Linux driver
- Userspace Rust crates

The CI pipeline at [`.github/workflows/release.yml`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/.github/workflows/release.yml)
runs on tag push, produces the artefacts listed below, and attaches
them to the GitHub Release.

## Release artefacts

| File                                         | Purpose                                    |
|----------------------------------------------|--------------------------------------------|
| `rpbridge-<ver>.uf2`                         | Firmware UF2 image.                        |
| `rpbridge-<ver>.uf2.minisig`                 | Ed25519 signature of the firmware.         |
| `rpbridge-dkms_<ver>_all.deb`                | Linux driver — Debian/Ubuntu DKMS package. |
| `rpbridge-dkms-<ver>-1.noarch.rpm`           | Linux driver — Fedora/Rocky DKMS package.  |
| `rpbridge-dkms-<ver>.tar.gz`                 | Distro-agnostic DKMS source tarball.       |
| `rpbridge-ctl-x86_64-linux-gnu`              | CLI binary, x86_64.                        |
| `rpbridge-ctl-aarch64-linux-gnu`             | CLI binary, aarch64.                       |
| `rpbridge-sbom.spdx.json`                    | SPDX SBOM.                                 |
| `CHECKSUMS.txt`                              | SHA-256 of every artefact.                 |
| `CHECKSUMS.txt.minisig`                      | Signature of the checksum manifest.        |

## Verifying signatures

```sh
minisign -Vm rpbridge-<ver>.uf2 -p rpbridge.pub
minisign -Vm CHECKSUMS.txt       -p rpbridge.pub
sha256sum -c CHECKSUMS.txt
```

The public key `rpbridge.pub` is published here and in
[`SECURITY.md`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/SECURITY.md)
at the first tagged release.

## Version compatibility

| Bump     | Effect on compatibility                        |
|----------|------------------------------------------------|
| PATCH    | Always compatible. Pure bugfix / doc release.  |
| MINOR    | Forward-compatible; older hosts keep working, but new features are only visible to new hosts. |
| MAJOR    | Breaking change. Accompanied by a migration guide in `CHANGELOG.md`. |

A running firmware N.x stays compatible with drivers (N-1).x and
(N+1).x as long as the MAJOR version matches — see
[ADR-0001](adr/0001-protocol-choice.md) §"Consequences".
