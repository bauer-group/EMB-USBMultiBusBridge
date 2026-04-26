// SPDX-License-Identifier: GPL-2.0
/*
 * Auxiliary-bus child glue — see aux.h for the lifetime contract.
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "auxbus.h"
#include "cap.h"
#include "rpbridge.h"

#define RPBRIDGE_MODNAME  "rpbridge"

/*
 * Tracking of spawned children so we can revoke them on disconnect
 * without walking the bus. Held under a per-device mutex (the same
 * rpbridge_dev::transport_lock would do, but kept separate so a
 * very-late aux callback inspecting the list cannot deadlock against
 * a transport in flight).
 */
struct aux_track {
	struct rpbridge_aux_dev *aux;
	struct list_head         link;
};

/*
 * Per-class allow-list. The capability map advertises a class string
 * (e.g. "gpio"); we only spawn children for classes the driver knows
 * how to bind, otherwise dmesg gets a single info line and we move
 * on. Forward-compat: a future firmware advertising "foo" simply
 * goes unbound until a driver is added here.
 */
static const char *const aux_supported_classes[] = {
	"gpio", "i2c", "spi", "pwm", "adc",
	"led", "dmx", "onewire", "i2s",
	NULL,
};

static bool class_supported(const char *cls)
{
	const char *const *p;

	if (!cls)
		return false;
	for (p = aux_supported_classes; *p; ++p)
		if (strcmp(cls, *p) == 0)
			return true;
	return false;
}

/*
 * Release callback — invoked by the device model after the last
 * reference on the auxiliary_device drops. Frees the wrapper and
 * releases the parent kref the spawn path took. Must NOT touch
 * any embedded struct that the kernel may still be inspecting
 * (the kobject is the only field guaranteed dead at this point).
 */
static void rpbridge_aux_release(struct device *dev)
{
	struct auxiliary_device *auxdev = to_auxiliary_dev(dev);
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);

	rpbridge_dev_put(aux->parent);
	aux->parent = NULL;
	kfree(aux);
}

/*
 * Tracking list lives in the parent's struct so we can scan it on
 * disconnect. We expose accessors through rpbridge.h to avoid
 * sprinkling list_head walks across files.
 */
static int track_aux(struct rpbridge_dev *dev, struct rpbridge_aux_dev *aux)
{
	struct aux_track *t;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->aux = aux;

	mutex_lock(&dev->aux_lock);
	list_add_tail(&t->link, &dev->aux_children);
	mutex_unlock(&dev->aux_lock);
	return 0;
}

static int spawn_one(struct rpbridge_dev *dev,
		     const struct rpbp_caps_bus *bus)
{
	struct rpbridge_aux_dev *aux;
	int ret;

	if (!class_supported(bus->class)) {
		dev_info(&dev->intf->dev,
			 "rpbridge: skipping unsupported class '%s' idx=%u\n",
			 bus->class, bus->idx);
		return 0;
	}

	aux = kzalloc(sizeof(*aux), GFP_KERNEL);
	if (!aux)
		return -ENOMEM;

	aux->parent  = rpbridge_dev_get(dev);
	aux->bus_idx = bus->idx;

	aux->auxdev.name           = bus->class;        /* "rpbridge.<cls>" via the modalias prefix */
	aux->auxdev.id             = bus->idx;
	aux->auxdev.dev.parent     = &dev->intf->dev;
	aux->auxdev.dev.release    = rpbridge_aux_release;

	ret = auxiliary_device_init(&aux->auxdev);
	if (ret) {
		/* On init failure the device model has not taken
		 * ownership yet, so we must release the parent kref
		 * and free the wrapper ourselves. */
		rpbridge_dev_put(aux->parent);
		kfree(aux);
		return ret;
	}

	ret = auxiliary_device_add(&aux->auxdev);
	if (ret) {
		/* After init() the release callback owns cleanup —
		 * uninit drops the device-model reference, the
		 * release fires, the parent kref + wrapper memory
		 * get freed by rpbridge_aux_release(). */
		auxiliary_device_uninit(&aux->auxdev);
		return ret;
	}

	ret = track_aux(dev, aux);
	if (ret) {
		auxiliary_device_delete(&aux->auxdev);
		auxiliary_device_uninit(&aux->auxdev);
		return ret;
	}
	return 0;
}

int rpbridge_spawn_aux_children(struct rpbridge_dev *dev)
{
	size_t i;
	int ret;

	if (!dev || !dev->caps)
		return -EINVAL;

	for (i = 0; i < dev->caps->bus_count; ++i) {
		ret = spawn_one(dev, &dev->caps->buses[i]);
		if (ret) {
			dev_err(&dev->intf->dev,
				"rpbridge: failed to spawn aux child for "
				"class='%s' idx=%u: %d (rolling back)\n",
				dev->caps->buses[i].class,
				dev->caps->buses[i].idx, ret);
			rpbridge_unregister_aux_children(dev);
			return ret;
		}
	}
	return 0;
}

void rpbridge_unregister_aux_children(struct rpbridge_dev *dev)
{
	struct aux_track *t, *tmp;

	if (!dev)
		return;

	mutex_lock(&dev->aux_lock);
	list_for_each_entry_safe(t, tmp, &dev->aux_children, link) {
		list_del(&t->link);
		mutex_unlock(&dev->aux_lock);

		auxiliary_device_delete(&t->aux->auxdev);
		auxiliary_device_uninit(&t->aux->auxdev);
		kfree(t);

		mutex_lock(&dev->aux_lock);
	}
	mutex_unlock(&dev->aux_lock);
}

/* ───── Per-subsystem stubs (real impls land in their own commits) ──── */

/* Each rpbridge_<subsys>_register() either lives in its own
 * subsystem .c (real implementation) or is a no-op stub here. As
 * each subsystem .c lands, its DEFINE_SUBSYS_STUB() line gets
 * removed below. */
#define DEFINE_SUBSYS_STUB(name) \
	int  rpbridge_##name##_register(void)   { return 0; } \
	void rpbridge_##name##_unregister(void) { }

/* led.c provides the real impl; dmx/onewire/i2s land in their own
 * commits and use stubs here until then so spawn_aux_children() can
 * still skip-or-bind the auxbus children for those classes. */
#define DEFINE_SUBSYS_STUB(name) \
	int  rpbridge_##name##_register(void)   { return 0; } \
	void rpbridge_##name##_unregister(void) { }

DEFINE_SUBSYS_STUB(onewire)
DEFINE_SUBSYS_STUB(i2s)
