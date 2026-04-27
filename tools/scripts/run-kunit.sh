#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Run KUnit tests for the RPBridge kernel driver against a local
# kernel source tree. The tree must have CONFIG_RUST=y and must have
# been configured with `make mrproper && make defconfig && scripts/config
# --enable CONFIG_RUST --enable CONFIG_AUXILIARY_BUS`.
#
# Usage:
#   KDIR=/path/to/linux tools/scripts/run-kunit.sh
#   KDIR=~/src/linux tools/scripts/run-kunit.sh

set -euo pipefail

KDIR="${KDIR:-$HOME/src/linux}"
if [ ! -d "$KDIR" ]; then
    echo "error: KDIR ($KDIR) does not exist. Set KDIR to a Rust-enabled kernel source tree." >&2
    exit 2
fi

cp -r driver "$KDIR/drivers/rpbridge.tmp"
( cd "$KDIR" && \
  ./tools/testing/kunit/kunit.py run \
    --arch=x86_64 \
    --kconfig_add=CONFIG_RPBRIDGE_KUNIT=y \
    rpbridge.* )

rm -rf "$KDIR/drivers/rpbridge.tmp"
