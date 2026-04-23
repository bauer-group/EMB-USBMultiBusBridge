# Security model

The full narrative lives in [`SECURITY.md`](https://github.com/bauer-group/USB-Multi-Bus-Bridge/blob/main/SECURITY.md).
Key points worth repeating in the docs:

1. **USB transport is not authenticated.** Physical access to the
   host is considered equivalent to physical access to the device.
2. **Firmware authenticity** is enforced by RP2350 signed-boot (OTP
   fuse) on production units and cross-checked by host-side
   `minisign` verification of UF2 artefacts.
3. **Wire integrity** is guarded by an application-level CRC-32C on
   every RPBP frame — catching firmware bugs and ESD transients in
   addition to the USB CRC-16.
4. **Attestation** (optional, feature-flagged) lets the host confirm
   the live firmware SHA-256 matches a signed attestation before
   trusting stream data for safety-critical use.
5. **Supply-chain hygiene** — pinned submodules, Dependabot-pinned
   Actions, SPDX SBOM attached to every release.

See [ADR-0001](../adr/0001-protocol-choice.md) for the CRC choice,
[ADR-0003](../adr/0003-language-choice.md) for the Rust-driven
memory-safety argument in the kernel driver.
