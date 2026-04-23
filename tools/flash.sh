#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Flash a UF2 onto a connected RPBridge. If the device is not already
# in BOOTSEL mode, try to trigger it via rpbridge-ctl; otherwise fall
# back to a clear instruction and wait for the user.
#
# Usage: tools/flash.sh [firmware.uf2]
#        tools/flash.sh          # defaults to firmware/build/rpbridge.uf2

set -euo pipefail

fw="${1:-firmware/build/rpbridge.uf2}"
if [ ! -f "$fw" ]; then
    echo "error: firmware image not found: $fw" >&2
    exit 2
fi

trigger_bootsel() {
    if command -v rpbridge-ctl >/dev/null; then
        echo "[+] triggering BOOTSEL via rpbridge-ctl"
        rpbridge-ctl reboot-bootsel || true
    else
        echo "[!] rpbridge-ctl not installed — hold BOOTSEL and replug the device."
    fi
}

wait_for_bootsel() {
    echo -n "[+] waiting for BOOTSEL device "
    for _ in $(seq 1 50); do
        if picotool info >/dev/null 2>&1; then
            echo "— ready"
            return 0
        fi
        sleep 0.2
        echo -n "."
    done
    echo " (timeout)"
    return 1
}

if ! picotool info >/dev/null 2>&1; then
    trigger_bootsel
    wait_for_bootsel || exit 3
fi

echo "[+] flashing $fw"
picotool load -fxv "$fw"
echo "[+] waiting for RPBridge re-enumeration"
sleep 1
lsusb | grep -i rpbridge || echo "   (device not seen by lsusb yet — give it a moment)"

echo "[+] done"
