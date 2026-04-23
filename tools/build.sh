#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Convenience wrapper that builds one or more components. Defers to
# the component's own build system — we do not reimplement their
# logic, we just point at sane defaults.
#
# Usage:
#   tools/build.sh                     # firmware + driver + userspace
#   tools/build.sh firmware
#   tools/build.sh driver
#   tools/build.sh userspace
#   tools/build.sh docs

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_root"

targets=("$@")
if [ ${#targets[@]} -eq 0 ]; then
    targets=(firmware driver userspace)
fi

build_firmware() {
    echo "[+] building firmware (pico-sdk, Release)"
    cmake -S firmware -B firmware/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build firmware/build --parallel
    echo "[+] firmware artefacts:"
    ls -1 firmware/build/rpbridge.*
}

build_driver() {
    echo "[+] building Linux kernel module (out-of-tree)"
    make -C driver
    echo "[+] module artefact:"
    ls -1 driver/*.ko 2>/dev/null || echo "    (build completed but no .ko — check dmesg / kernel config)"
}

build_userspace() {
    echo "[+] building userspace Rust workspace (release)"
    ( cd userspace && cargo build --release --workspace )
    echo "[+] CLI artefact:"
    ls -1 userspace/target/release/rpbridge-ctl
}

build_docs() {
    echo "[+] building documentation site"
    ( cd docs && mkdocs build --strict )
}

for t in "${targets[@]}"; do
    case "$t" in
        firmware)  build_firmware  ;;
        driver)    build_driver    ;;
        userspace) build_userspace ;;
        docs)      build_docs      ;;
        *) echo "unknown target: $t" >&2; exit 2 ;;
    esac
done
