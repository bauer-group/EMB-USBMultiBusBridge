#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# umockdev replay driver for rpbridge.ko. Loads the module against a
# recorded RPBridge enumeration and asserts the expected aux_device
# children appear under /sys/bus/auxiliary/devices.
#
# Usage:
#   ./replay.sh /path/to/rpbridge.ko
#
# Exit codes:
#   0   all expected children present (and nothing unexpected)
#   1   umockdev / tooling missing or wrong version
#   2   expected child missing
#   3   unexpected extra child

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
module="${1:?path to rpbridge.ko required}"

# ─── Preflight ────────────────────────────────────────────────────────

if ! command -v umockdev-run >/dev/null 2>&1; then
    echo "error: umockdev-run not found. Install umockdev >= 0.17." >&2
    exit 1
fi

if [[ ! -r "$module" ]]; then
    echo "error: $module is not readable" >&2
    exit 1
fi

recording="$here/rpbridge-attach.umockdev"
ioctls="$here/rpbridge-attach.ioctl"
expected="$here/expected-children.txt"

for f in "$recording" "$ioctls" "$expected"; do
    if [[ ! -f "$f" ]]; then
        echo "error: missing test fixture: $f" >&2
        echo "       regenerate with the steps in README.md" >&2
        exit 1
    fi
done

# ─── Replay ───────────────────────────────────────────────────────────

echo "==> loading $module under umockdev-run"
sudo umockdev-run \
    --device "$recording" \
    --ioctl  "$ioctls" \
    -- bash -c "insmod '$module' && sleep 1 && \
                ls /sys/bus/auxiliary/devices/ 2>/dev/null \
                    | sort -u > /tmp/rpbridge-aux-children.txt; \
                rmmod rpbridge || true"

got=$(sort -u /tmp/rpbridge-aux-children.txt 2>/dev/null || true)
want=$(grep -v '^#' "$expected" | sort -u)

# ─── Compare ──────────────────────────────────────────────────────────

missing=$(comm -23 <(echo "$want") <(echo "$got") || true)
extra=$(comm -13 <(echo "$want") <(echo "$got") || true)

if [[ -n "$missing" ]]; then
    echo "error: expected aux_device children missing:" >&2
    echo "$missing" | sed 's/^/    /' >&2
    exit 2
fi
if [[ -n "$extra" ]]; then
    echo "error: unexpected aux_device children:" >&2
    echo "$extra" | sed 's/^/    /' >&2
    exit 3
fi

echo "==> replay passed: all $(echo "$want" | wc -l) expected children present"
exit 0
