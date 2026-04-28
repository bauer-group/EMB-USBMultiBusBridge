# Driver — DKMS packaging

The driver ships as a **DKMS source package**. DKMS rebuilds the
module every time the user upgrades their kernel, so the binary
track keeps up with distribution kernel bumps without manual
intervention.

Source layout for packaging:

- [`driver/dkms.conf.in`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/dkms.conf.in)
  — DKMS recipe template. The `@VERSION@` token is filled in by the
  packaging step.
- [`driver/packaging/debian/`](https://github.com/bauer-group/EMB-USBMultiBusBridge/tree/main/driver/packaging/debian)
  — `debhelper` recipe. Builds `rpbridge-dkms_<ver>_all.deb`.
- [`driver/packaging/rpm/rpbridge.spec`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/driver/packaging/rpm/rpbridge.spec)
  — RPM spec, targets Fedora ≥ 41 and Rocky/RHEL ≥ 9.

CI pipelines build both formats on tag push via
[`.github/workflows/release.yml`](https://github.com/bauer-group/EMB-USBMultiBusBridge/blob/main/.github/workflows/release.yml)
and attach them to the GitHub Release.

## Local DKMS install

```sh
# from the repo root, with the source tree checked out:
sudo dkms add     driver/                          # registers /usr/src/rpbridge-0.1.0/
sudo dkms build   rpbridge/0.1.0                   # compiles against running kernel
sudo dkms install rpbridge/0.1.0                   # → /lib/modules/.../updates/dkms/

# udev rule + module load (DKMS doesn't ship the udev rule on its own):
sudo cp driver/udev/70-rpbridge.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo modprobe rpbridge

dkms status                                        # verify "installed"
```

`sudo dkms autoinstall` re-runs the build after a kernel upgrade and
reinstalls the module automatically. The CI-built `.deb` / `.rpm`
packages do all of this in their post-install hook (including the
udev rule drop) — only the manual-source path needs the explicit
`cp` step above.

## Verifying a package

```sh
dpkg-deb --info rpbridge-dkms_0.1.0_all.deb
rpm --info rpbridge-dkms-0.1.0-1.noarch.rpm
```

Both package manifests recommend (not require) `libgpiod`,
`i2c-tools`, `can-utils` so the user's first-login experience can
include scanning buses immediately after install.
