/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Auxiliary-bus child glue. Each advertised subsystem
 * (gpio / i2c / spi / pwm / iio / led / dmx / onewire / i2s) gets
 * its own auxiliary_device under the parent USB device. The matching
 * auxiliary_driver is registered from rpbridge_module_init() and
 * binds via the aux_device::name field — "rpbridge.<class>".
 *
 * Lifetime: aux children take an extra reference on the parent
 * rpbridge_dev so the per-subsystem callback paths can deref a
 * stable pointer even if the USB disconnect runs concurrently.
 * The reference is dropped in the aux_device::release callback.
 */

#ifndef RPBRIDGE_AUX_H
#define RPBRIDGE_AUX_H

#include <linux/auxiliary_bus.h>

struct rpbridge_dev;
struct rpbp_caps_bus;

/**
 * struct rpbridge_aux_dev - wrapper around a struct auxiliary_device.
 *
 * @auxdev: the embedded auxiliary_device. Its dev->release is wired
 *          to rpbridge_aux_release(); never call kfree() on this
 *          struct directly — let the device-model finish first.
 * @parent: borrowed reference to the parent rpbridge_dev — incremented
 *          via rpbridge_dev_get() in spawn(), decremented in release.
 *          Always non-NULL between spawn() and release.
 * @bus_idx: subsystem-instance index from the capability map (e.g.
 *           bus 0 vs bus 1 for a board with two I²C controllers).
 */
struct rpbridge_aux_dev {
	struct auxiliary_device  auxdev;
	struct rpbridge_dev     *parent;
	u16                      bus_idx;
};

/**
 * rpbridge_spawn_aux_children() - walk dev->caps->buses and create one
 * auxiliary_device per advertised subsystem instance.
 *
 * Allocates and registers each child; on partial failure unwinds the
 * already-spawned ones and returns the first error. dev->caps must
 * be populated before this is called (probe path runs cap-fetch first).
 *
 * Returns 0 on success or a negative errno.
 */
int rpbridge_spawn_aux_children(struct rpbridge_dev *dev);

/**
 * rpbridge_unregister_aux_children() - remove every auxiliary_device
 * spawned by rpbridge_spawn_aux_children() for @dev. Safe to call
 * multiple times.
 */
void rpbridge_unregister_aux_children(struct rpbridge_dev *dev);

/* Subsystem-specific auxiliary_driver registration entry points.
 * Each <subsys>_register() registers an auxiliary_driver on
 * module-load, <subsys>_unregister() reverses it on module-unload.
 * Stubs return 0 / no-op until that subsystem's child driver lands. */

int  rpbridge_gpio_register(void);
void rpbridge_gpio_unregister(void);
int  rpbridge_i2c_register(void);
void rpbridge_i2c_unregister(void);
int  rpbridge_spi_register(void);
void rpbridge_spi_unregister(void);
int  rpbridge_pwm_register(void);
void rpbridge_pwm_unregister(void);
int  rpbridge_iio_register(void);
void rpbridge_iio_unregister(void);
int  rpbridge_led_register(void);
void rpbridge_led_unregister(void);
int  rpbridge_dmx_register(void);
void rpbridge_dmx_unregister(void);
int  rpbridge_onewire_register(void);
void rpbridge_onewire_unregister(void);
int  rpbridge_i2s_register(void);
void rpbridge_i2s_unregister(void);

#endif /* RPBRIDGE_AUX_H */
