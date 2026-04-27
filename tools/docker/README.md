# RPBridge build container

A single Ubuntu 26.04 LTS image carries the full toolchain for every
component in this tree:

| Component | Tooling |
| --- | --- |
| Firmware | ARM GNU Toolchain (`gcc-arm-none-eabi`), CMake, Ninja, Pico-SDK 2.2.0 (vendored as a submodule) |
| Driver | Linux 6.18 headers (`linux-headers-generic`), distro `rustc` matching the kernel pin, `bindgen`, `clang` / `lld` / `llvm` |
| Userspace | rustup stable + cross-target `aarch64-unknown-linux-gnu`, `gcc-aarch64-linux-gnu`, libudev / libusb |
| Driver tests | QEMU (x86 + arm) for KUnit-in-VM runs |

## Why Ubuntu 26.04 LTS

The driver's Rust-for-Linux module needs kernel ≥ 6.18 with
`CONFIG_RUST=y` and a `rustc` matching the kernel's
`scripts/min-tool-version.sh` pin. Ubuntu 26.04 LTS is the first
mainstream distro that ships all three out of the box — which means
the Dockerfile is `apt install …` and nothing more, no backports
gymnastics, no manual kernel-tarball extraction.

Debian 13 Trixie was the alternative; it ships kernel 6.12 by default
and only offers 6.18 via backports, which would have meant
`apt-pinning` against `trixie-backports` and a more brittle setup.

## Why no image pinning / no registry

This project deliberately rebuilds the image on every CI run and on
each developer machine, with floating apt versions. The trade-off:

* **Local and CI agree** within a build cycle, because both pull the
  same package versions from the same archive at the same time.
  This was the original problem (`lokal nicht sauber, remote
  komplett anders`) and it is fully solved.
* **Cross-time byte-equality** is not preserved — rebuilding any
  given release tag six months later may yield a binary that
  differs from the original release. For embedded products with
  regulatory reproducibility requirements (medical, automotive
  ASIL), this trade-off is wrong; switch to digest-pinned images
  and a registry before going to production in those domains.

## Layer-cache behaviour

Subsequent runs of `tools/build.sh` re-execute `docker build`, but
Docker's layer cache makes this essentially free as long as the
Dockerfile and `tools/docker/.dockerignore` are unchanged. The first
invocation on a fresh machine takes 5–10 minutes (apt + rustup); every
subsequent invocation is sub-second.

GitHub Actions runners get the same speedup via `docker/build-push-action`'s
GHA cache backend (`cache-from: type=gha, cache-to: type=gha,mode=max`),
configured in `.github/workflows/firmware.yml` and friends.
