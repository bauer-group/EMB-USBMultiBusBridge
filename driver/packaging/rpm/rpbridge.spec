# SPDX-License-Identifier: GPL-2.0
#
# RPM spec for the rpbridge DKMS source package. Targets Fedora ≥ 41
# and Rocky/RHEL ≥ 9. Both distributions now ship kernels with
# CONFIG_RUST=y by default; on older distributions the module will
# refuse to build (the wrapper Makefile bails out with a clear error).

%global modname rpbridge
%global modver  %{?version}%{!?version:0.1.0}

Name:           rpbridge-dkms
Version:        %{modver}
Release:        1%{?dist}
Summary:        RPBridge USB multi-bus bridge Rust kernel driver (DKMS)

License:        GPL-2.0
URL:            https://github.com/bauer-group/USB-Multi-Bus-Bridge
Source0:        %{name}-%{version}.tar.gz

BuildArch:      noarch
BuildRequires:  dkms
Requires:       dkms >= 3.0
Requires:       kernel-devel
Recommends:     libgpiod
Recommends:     i2c-tools
Recommends:     can-utils

%description
RPBridge is an open-source USB-to-multi-bus developer tool built on the
Raspberry Pi RP2354B. This package contains the Rust-for-Linux kernel
module, packaged for DKMS. A kernel with CONFIG_RUST=y is required.

%prep
%setup -q -n %{name}-%{version}

%build
# nothing — DKMS will invoke `make` inside the source tree at
# install / kernel-upgrade time.

%install
install -d %{buildroot}%{_usrsrc}/%{modname}-%{version}
cp -a driver/. %{buildroot}%{_usrsrc}/%{modname}-%{version}/

install -D -m 0644 driver/udev/70-rpbridge.rules \
    %{buildroot}%{_prefix}/lib/udev/rules.d/70-rpbridge.rules
install -D -m 0644 driver/modprobe/rpbridge.conf \
    %{buildroot}%{_sysconfdir}/modprobe.d/rpbridge.conf

sed -i "s/@VERSION@/%{version}/g" \
    %{buildroot}%{_usrsrc}/%{modname}-%{version}/dkms.conf.in

%post
dkms add -m %{modname} -v %{version} || :
dkms build -m %{modname} -v %{version} || :
dkms install -m %{modname} -v %{version} || :

%preun
if [ $1 -eq 0 ]; then
    dkms remove -m %{modname} -v %{version} --all || :
fi

%files
%license LICENSE
%doc README.md CHANGELOG.md
%{_usrsrc}/%{modname}-%{version}
%{_prefix}/lib/udev/rules.d/70-rpbridge.rules
%config(noreplace) %{_sysconfdir}/modprobe.d/rpbridge.conf

%changelog
* Wed Apr 22 2026 BAUER GROUP <kb@de.bauer-group.com> - 0.1.0-1
- Initial packaging of the rpbridge Rust kernel driver as DKMS source.
