#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Multi-board pin discipline check.
#
# Fails if firmware/src/ contains hardcoded GP-pin integer literals
# as the first argument to pico-sdk functions that take a GPIO pin
# number. Every such call must use a RPBRIDGE_*_PIN macro from
# firmware/boards/rpbridge_*.h so a board variant can override the
# pin without an out-of-tree source patch (M11 invariant).
#
# Inline opt-out:  append `// pin-discipline: ok` on the same line
#                  for the rare case where a literal is intentional
#                  (e.g. PIO clock-pin enumeration in a vendor block).
#
# Usage:  ./firmware/scripts/check-pin-discipline.sh
# Returns 0 if clean, 1 if violations are found.

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
src_dirs=("$repo_root/firmware/src")

# Pin-typed pico-sdk functions: first arg is a GPIO number.
# Listed explicitly so unrelated functions taking integer arguments
# (e.g. dma_channel_claim) are NOT swept up.
fns=(
    gpio_init
    gpio_init_mask
    gpio_deinit
    gpio_set_dir
    gpio_set_function
    gpio_pull_up
    gpio_pull_down
    gpio_disable_pulls
    gpio_set_input_enabled
    gpio_set_input_hysteresis_enabled
    gpio_set_slew_rate
    gpio_set_drive_strength
    gpio_put
    gpio_get
    gpio_set_irq_enabled
    gpio_set_irq_enabled_with_callback
    pwm_gpio_to_slice_num
    pwm_gpio_to_channel
    pwm_set_gpio_level
    adc_gpio_init
    adc_select_input
    pio_gpio_init
)

# Build the alternation pattern, e.g.  \b(gpio_init|gpio_set_dir|...)\s*\(\s*[0-9]+
ifs_pattern=$(IFS='|'; echo "${fns[*]}")
pattern="\\b(${ifs_pattern})\\s*\\(\\s*[0-9]+"

violations=0
matches=$(grep -rnE "$pattern" --include='*.c' --include='*.h' \
              "${src_dirs[@]}" 2>/dev/null || true)

if [ -n "$matches" ]; then
    while IFS= read -r line; do
        # Honour inline opt-out comment.
        if echo "$line" | grep -q 'pin-discipline: ok'; then
            continue
        fi
        echo "::error::pin discipline: $line"
        violations=$((violations + 1))
    done <<< "$matches"
fi

if (( violations > 0 )); then
    echo
    echo "::error::$violations hardcoded pin literal(s) in firmware/src/."
    echo "Use RPBRIDGE_*_PIN macros from firmware/boards/. To bypass"
    echo "intentionally on a single line, append: // pin-discipline: ok"
    exit 1
fi

echo "[check-pin-discipline] clean (${#fns[@]} pin-typed functions audited)"
