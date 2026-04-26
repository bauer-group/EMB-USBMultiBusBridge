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

CI pipelines build both formats on tag push
(`.github/workflows/driver-package.yml`).

## Local DKMS install

```sh
sudo dkms add    driver
sudo dkms build  rpbridge/0.1.0
sudo dkms install rpbridge/0.1.0
sudo modprobe rpbridge

dkms status
```

`sudo dkms autoinstall` re-runs the build after a kernel upgrade and
reinstalls the module automatically.

## Verifying a package

```sh
dpkg-deb --info rpbridge-dkms_0.1.0_all.deb
rpm --info rpbridge-dkms-0.1.0-1.noarch.rpm
```

Both package manifests recommend (not require) `libgpiod`,
`i2c-tools`, `can-utils` so the user's first-login experience can
include scanning buses immediately after install.
