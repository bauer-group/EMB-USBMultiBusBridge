# Firmware — OTA update (BOOTSEL / UF2)

RP2350 ships an unchangeable ROM mass-storage bootloader. We rely on
it exclusively — no custom bootloader, no A/B slots, no signing
ceremony just to flash.

## Flow

1. Host issues `rpbridge-ctl reboot-bootsel` (or writes `1` to
   `/sys/bus/usb/drivers/rpbridge/<dev>/reboot_bootsel`).
2. Firmware calls `reset_usb_boot()` and re-enumerates as `RP2350`
   mass-storage + `PICOBOOT` vendor device.
3. Host flashes via `picotool load -fxv rpbridge.uf2` or drags the
   `.uf2` onto the mounted volume.
4. Device boots the new image and re-enumerates as RPBridge.

## Integrity

- UF2 artefacts are signed with `minisign` (Ed25519); the public
  key is embedded in `rpbridge-ctl`.
- `rpbridge-ctl flash` verifies the signature **before** rebooting
  into BOOTSEL — a bad signature aborts without touching the device.
- Optional post-flash attestation (`CAPABILITIES.attestation == true`)
  confirms the running firmware SHA-256 matches expectations.

## Userspace one-liner

```sh
rpbridge-ctl flash firmware/build/rpbridge.uf2
```

Under the hood the tool:

- verifies `rpbridge.uf2.minisig`,
- sends `SUBSYS_SYS / OP_REBOOT_BOOTSEL` over RPBP,
- waits for BOOTSEL re-enumeration (polls udev),
- runs `picotool load -fxv`,
- waits for the RPBridge re-enumeration,
- queries capabilities to confirm version.

The whole round trip completes in under 3 seconds on a modern host.
