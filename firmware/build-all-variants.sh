#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build every supported RPBridge firmware variant in one go.
# Output artefacts land under ./build/<variant>/rpbridge-<variant>.uf2.
#
# Usage:
#   ./firmware/build-all-variants.sh           # build all variants
#   ./firmware/build-all-variants.sh pico2     # build a single variant
#
# The CI workflow at .github/workflows/firmware.yml uses this same
# loop with each board passed as a matrix axis.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Keep this list in sync with RPBRIDGE_SUPPORTED_BOARDS in
# firmware/CMakeLists.txt — the CMake check rejects unknown boards.
ALL_VARIANTS=(rpbridge_rp2354b rpbridge_pico2)

if [[ $# -gt 0 ]]; then
    VARIANTS=()
    for arg in "$@"; do
        # Allow short forms ("pico2" → "rpbridge_pico2")
        case "$arg" in
            rpbridge_*) VARIANTS+=("$arg") ;;
            *)          VARIANTS+=("rpbridge_$arg") ;;
        esac
    done
else
    VARIANTS=("${ALL_VARIANTS[@]}")
fi

# Pick the same generator CI uses (Ninja). On Windows without VS, the
# default of "NMake Makefiles" requires nmake which isn't part of the
# pico-sdk toolchain. Ninja is shipped alongside it (~/.pico-sdk/ninja/).
GENERATOR="${RPBRIDGE_CMAKE_GENERATOR:-Ninja}"

# Multi-board pin-discipline gate: catches hardcoded GP literals in
# firmware/src/ that should go through RPBRIDGE_*_PIN macros instead.
# Skip with RPBRIDGE_SKIP_PIN_DISCIPLINE=1 in the rare case where the
# script itself needs to be debugged from the build flow.
if [[ "${RPBRIDGE_SKIP_PIN_DISCIPLINE:-0}" != "1" ]]; then
    bash "$here/scripts/check-pin-discipline.sh"
fi

for variant in "${VARIANTS[@]}"; do
    build_dir="$here/build/${variant#rpbridge_}"
    echo
    echo "==> $variant — configuring in $build_dir (generator: $GENERATOR)"
    cmake -S "$here" -B "$build_dir" \
        -G "$GENERATOR" \
        -DPICO_BOARD="$variant" \
        -DPICO_PLATFORM=rp2350 \
        -DCMAKE_BUILD_TYPE=Release

    echo "==> $variant — building"
    cmake --build "$build_dir" --parallel

    artefact="$build_dir/rpbridge-${variant#rpbridge_}.uf2"
    if [[ -f "$artefact" ]]; then
        echo "==> $variant — wrote $artefact ($(stat -c%s "$artefact" 2>/dev/null || stat -f%z "$artefact") bytes)"
    else
        echo "==> $variant — WARNING: expected artefact $artefact not found" >&2
    fi
done

echo
echo "==> all variants built successfully"
