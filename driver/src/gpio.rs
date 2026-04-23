// SPDX-License-Identifier: GPL-2.0
//
// rpbridge-gpio — auxiliary child driver that registers a gpio_chip.
//
// Exposes the RPBridge's GPIO pins via `/dev/gpiochipN` using the
// GPIO character-device ABI v2 (libgpiod 2.x compatible). sysfs GPIO
// is deliberately NOT implemented — it has been deprecated since 4.8
// and we do not carry legacy surfaces in a new driver.
//
// Implementation bullet list (see firmware/TODO.md §M3 for the matching
// firmware side):
//   - `kernel::gpio::Chip` with get/set, direction control, pull config
//   - `kernel::gpio::IrqChip` as nested irq_chip for edge/level IRQs
//   - debounce via a firmware-side parameter, not kernel software
//   - pin-count and per-pin capability bitmap read from crate::cap
//
// The stub below just records that it was probed, so the kernel can
// bind it during smoke tests before the real implementation lands.

use kernel::prelude::*;

#[allow(dead_code)]
pub(crate) struct GpioChild;

#[allow(dead_code)]
impl GpioChild {
    pub(crate) fn probe() -> Result<Self> {
        pr_info!("rpbridge-gpio: probe (stub)\n");
        Ok(GpioChild)
    }
}
