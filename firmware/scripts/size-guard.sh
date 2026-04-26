#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Soft cap on flash + static-RAM footprint. Catches creep before it
# reaches the linker's hard cap (= physical flash) so the regression
# surfaces at PR review, not when a customer flashes a brick.
#
# Today (M11): ~52 KiB flash + ~52 KiB RAM static. Defaults give ~5×
# headroom; bump deliberately when a feature genuinely needs it.
#
# Usage:
#   firmware/scripts/size-guard.sh <board>
#
# Environment overrides:
#   RPBRIDGE_FLASH_BUDGET_KB   default 256
#   RPBRIDGE_RAM_BUDGET_KB     default 96

set -euo pipefail

board="${1:-}"
[[ -n "$board" ]] || { echo "usage: $0 <board>" >&2; exit 2; }

short="${board#rpbridge_}"
build_dir="firmware/build/$short"
base="rpbridge-$short"
elf="$build_dir/$base.elf"
map="$build_dir/$base.elf.map"

[[ -f "$elf" ]] || { echo "::error::no ELF at $elf" >&2; exit 1; }
[[ -f "$map" ]] || { echo "::error::no map at $map" >&2; exit 1; }

flash_budget_kb="${RPBRIDGE_FLASH_BUDGET_KB:-256}"
ram_budget_kb="${RPBRIDGE_RAM_BUDGET_KB:-96}"

ram_used=$(arm-none-eabi-size -A "$elf" \
    | awk '/^\.data |^\.bss |^\.ram_vector_table /{s+=$2} END{print s+0}')

# Flash footprint = __flash_binary_end - FLASH origin (0x10000000).
# Map-file format varies between binutils versions; both forms grep
# the same `0x10…` address out of the line carrying the symbol.
flash_addr_hex=$(grep -E '__flash_binary_end[[:space:]]*=' "$map" \
                 | grep -oE '0x10[0-9a-fA-F]+' | head -1)
if [[ -z "$flash_addr_hex" ]]; then
    echo "::error::could not parse __flash_binary_end from $map" >&2
    exit 1
fi
# Bash arithmetic understands 0x-prefixed hex literals natively — no
# gawk strtonum / python needed, so we don't depend on extension awks.
flash_used=$(( flash_addr_hex - 0x10000000 ))

flash_kb=$(( flash_used / 1024 ))
ram_kb=$(( ram_used / 1024 ))

{
    echo
    echo "── Size guard ────────────────────────────────────────"
    printf "Board            : %s\n" "$board"
    arm-none-eabi-size --format=berkeley "$elf"
    echo
    printf "Flash footprint : %6d B (%4d KiB)  budget %4d KiB\n" \
        "$flash_used" "$flash_kb" "$flash_budget_kb"
    printf "Static RAM       : %6d B (%4d KiB)  budget %4d KiB\n" \
        "$ram_used"  "$ram_kb"   "$ram_budget_kb"
} | tee -a "$build_dir/size.txt"

rc=0
if (( flash_kb > flash_budget_kb )); then
    echo "::error::Flash footprint ${flash_kb} KiB exceeds budget ${flash_budget_kb} KiB."
    rc=1
fi
if (( ram_kb > ram_budget_kb )); then
    echo "::error::Static RAM ${ram_kb} KiB exceeds budget ${ram_budget_kb} KiB."
    rc=1
fi
exit "$rc"
