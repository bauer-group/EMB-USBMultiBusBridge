# Licensing

| Component                               | License                 |
|------------------------------------------|-------------------------|
| Firmware (`firmware/`)                  | **BSD-3-Clause** (pico-sdk-compatible) |
| Linux kernel driver (`driver/`)         | **GPL-2.0** (kernel requirement) |
| Userspace Rust crates (`userspace/`)    | **Apache-2.0 OR MIT** (Rust default) |
| Documentation (`docs/`)                 | **CC-BY-4.0**          |
| Hardware design files (`hardware/`)     | **CERN-OHL-S-2.0**     |
| Example scripts (`tools/`, tutorials)   | **Apache-2.0 OR MIT**  |
| Vendored third-party (`third_party/`)   | upstream (per submodule): pico-sdk BSD-3, can2040 GPL-3 |

Every source file carries the SPDX identifier for its license; the
top-level `LICENSE` file reproduces the project-default license. The
full SPDX expression per file is authoritative.

## Why the mixed model

- The Linux kernel forces GPL-2.0 on any module that is linked into
  the kernel at runtime.
- pico-sdk is BSD-3-Clause and we follow the same license on the
  firmware side to stay compatible with reused SDK components.
- Rust userspace uses the dual Apache/MIT convention that is almost
  universal in the crate ecosystem.
- Hardware designs use CERN-OHL-S-2.0, the strong-reciprocity
  hardware license, to keep derivative board designs open.

## Contributing under these licenses

By opening a pull request against this repository, contributors agree
to license their contributions under the license applicable to the
file(s) they touch. Mixed changes are licensed per-file.

Sign-off (`git commit -s`) is mandatory on kernel-driver changes for
eventual upstream submission; the BAUER GROUP commit standard
**forbids** AI-attribution trailers in any commit.
