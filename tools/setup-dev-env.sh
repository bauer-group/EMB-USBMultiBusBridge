#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Install the host-side toolchains the RPBridge project expects.
# Idempotent — re-running upgrades versions where necessary.
#
# Supported:
#   - Debian 12, Ubuntu 22.04 / 24.04 / 25.04
#   - Fedora 40+
#
# Usage:  ./tools/setup-dev-env.sh
#         ./tools/setup-dev-env.sh --skip-rust    # if already installed

set -euo pipefail

SKIP_RUST=0
SKIP_MKDOCS=0
for arg in "$@"; do
    case "$arg" in
        --skip-rust)    SKIP_RUST=1 ;;
        --skip-mkdocs)  SKIP_MKDOCS=1 ;;
        -h|--help)
            sed -n '2,15p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

detect_pm() {
    if   command -v apt-get >/dev/null; then echo apt
    elif command -v dnf     >/dev/null; then echo dnf
    elif command -v pacman  >/dev/null; then echo pacman
    else echo "unsupported package manager" >&2; exit 3
    fi
}

install_apt() {
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config \
        gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
        python3 python3-pip python3-venv \
        libusb-1.0-0-dev libudev-dev \
        libgpiod2 libgpiod-dev \
        i2c-tools can-utils \
        dkms \
        git curl ca-certificates
}

install_dnf() {
    sudo dnf install -y \
        @development-tools cmake ninja-build pkgconfig \
        arm-none-eabi-gcc-cs arm-none-eabi-newlib arm-none-eabi-binutils-cs \
        python3 python3-pip \
        libusbx-devel libudev-devel \
        libgpiod-devel i2c-tools can-utils \
        dkms kernel-devel \
        git curl
}

install_pacman() {
    sudo pacman -S --needed --noconfirm \
        base-devel cmake ninja \
        arm-none-eabi-gcc arm-none-eabi-newlib arm-none-eabi-binutils \
        python python-pip \
        libusb libudev0-shim \
        libgpiod i2c-tools can-utils \
        dkms linux-headers \
        git curl
}

install_rust() {
    if command -v cargo >/dev/null && [ "$SKIP_RUST" -eq 0 ]; then
        echo "[+] rustup already installed; running update"
        rustup update stable
    elif [ "$SKIP_RUST" -eq 0 ]; then
        echo "[+] installing rustup"
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
        # shellcheck source=/dev/null
        source "$HOME/.cargo/env"
    fi
    rustup component add rustfmt clippy
    rustup target  add aarch64-unknown-linux-gnu || true
}

install_mkdocs() {
    if [ "$SKIP_MKDOCS" -eq 1 ]; then return; fi
    python3 -m pip install --user --upgrade \
        "mkdocs>=1.6" \
        "mkdocs-material>=9.5" \
        "mkdocs-git-revision-date-localized-plugin>=1.2" \
        "mike>=2.1" \
        "pymdown-extensions>=10.8"
}

main() {
    case "$(detect_pm)" in
        apt)    install_apt ;;
        dnf)    install_dnf ;;
        pacman) install_pacman ;;
    esac
    install_rust
    install_mkdocs

    echo
    echo "[+] toolchain versions:"
    cmake --version | head -n 1
    ninja --version || true
    arm-none-eabi-gcc --version | head -n 1
    rustc --version
    python3 -V
    echo
    echo "[+] Done. Rerun with --skip-rust / --skip-mkdocs to save time."
}

main "$@"
