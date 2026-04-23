# Security Policy

## Scope

This document covers vulnerabilities and security-impacting defects in:

* the RPBridge firmware (`firmware/`),
* the Linux kernel driver (`driver/`),
* the Rust userspace crates (`userspace/`),
* the release artifacts distributed from this repository or from
  `rpbridge.org`.

Hardware-level attacks that require physical modification of the device
(OTP-fuse tampering, die decap, side-channel analysis with access to
the PCB) are outside the scope we can defend against on an open, USB
bus-powered developer tool. See the threat model in
[`docs/drafts/rpbridge-protocol-and-modern-stack.md`](docs/drafts/rpbridge-protocol-and-modern-stack.md)
§6 for the full list.

## Reporting a vulnerability

**Please do not open public issues for security-sensitive bugs.**

Email: `security@bauer-group.com` (preferred) or `kb@de.bauer-group.com`
direct.

Optional: encrypt with the maintainer's age/PGP key published at
`https://rpbridge.org/.well-known/security.txt` (to be published at V1.0).

Expected acknowledgement: **within 3 working days.** We will keep you
informed about triage, fix, and disclosure timeline. We target a
coordinated-disclosure window of **90 days** from report to public
advisory, extended if the fix requires upstream-kernel coordination.

## Release-artifact integrity

All firmware, driver, and userspace release artifacts are signed with
**minisign** (Ed25519). The public key is:

```text
RWQ+g7…      <-- real key published at first tagged release
```

and is reproduced in `docs/releases.md`. Verify before installing:

```sh
minisign -Vm rpbridge.uf2 -p rpbridge.pub
minisign -Vm rpbridge-dkms_*.deb -p rpbridge.pub
minisign -Vm rpbridge-ctl-x86_64-linux-gnu -p rpbridge.pub
```

CI-generated checksums additionally live in `CHECKSUMS.txt` next to the
release artifacts.

## In-device firmware verification

For production builds the RP2350 OTP-fused public-key hash enforces
signed-boot. Community self-builds can leave the fuse blank (secure-boot
is optional). See [`docs/firmware/secure-boot.md`](docs/firmware/secure-boot.md)
for details.

## Supply-chain hygiene

* All third-party Rust crates are pinned in `Cargo.lock` (committed).
* All GitHub Actions are pinned to commit SHA via Dependabot
  (`.github/dependabot.yml`).
* Firmware submodules (`pico-sdk`, `can2040`) are pinned to specific
  tags/commits in `.gitmodules`.
* An SPDX SBOM is attached to every release via the Anchore action.

## Known-issue disclosure cadence

* High/critical: advisory within 7 days of fix landing.
* Medium: rolled up in the next minor release notes.
* Low / hardening: noted in the changelog.

## Cryptographic algorithm choices

* Firmware signing: **Ed25519** (via minisign on the host; RP2350 ROM
  signed-boot supports SHA-256+RSA-PSS-2048 or SHA-256+secp256k1 —
  we use SHA-256+secp256k1 for the in-ROM check because it matches the
  RP2350 signed-boot defaults).
* Wire-level integrity: **CRC-32C** (Castagnoli) additionally to USB's
  own CRC-16. This catches firmware-level bit flips, not adversaries —
  USB is not an authenticated transport.
* Attestation (optional feature): Ed25519 over host-nonce + firmware
  SHA-256.

No TLS or DTLS on the USB bulk endpoint — physical access to USB is
equivalent to physical access to the device, and the added attack
surface of a TLS stack on a Cortex-M33 is not justified.

## Contact

| Channel           | Address                                        |
|-------------------|------------------------------------------------|
| Security (PGP)    | `security@bauer-group.com`                     |
| Maintainer direct | `kb@de.bauer-group.com`                        |
| GitHub advisory   | Use the *Security* tab on the repository page. |
