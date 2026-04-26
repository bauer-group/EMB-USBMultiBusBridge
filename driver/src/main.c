// SPDX-License-Identifier: GPL-2.0
/*
 * RPBridge USB multi-bus bridge driver — entry point.
 *
 * Lifetime model (defensive, no leaks on any failure path):
 *
 *   probe():
 *     1. usb_get_dev()      — strong ref on the parent USB device
 *     2. kzalloc()          — driver-private state; kref_init() to 1
 *     3. find bulk pair     — fail releases dev + state
 *     4. mutex_init()       — both transport_lock and seq_lock
 *     5. usb_set_intfdata() — kernel can find us back on disconnect
 *     6. (future) hello + caps + spawn aux children
 *
 *   disconnect():
 *     1. usb_set_intfdata(NULL)  — block any latent callbacks
 *     2. (future) unregister aux children
 *     3. rpbridge_dev_put()       — releases when last ref drops
 *
 * Every error path in probe() unwinds in reverse order. The kref
 * release callback is the single chokepoint that frees state +
 * usb_put_dev() the parent — guaranteeing exactly-once cleanup
 * even if probe and disconnect race (USB core serialises them in
 * practice, but the code does not rely on that).
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "auxbus.h"
#include "cap.h"
#include "rpbp.h"
#include "rpbridge.h"
#include "session.h"

/* ───── Module metadata ──────────────────────────────────────────── */

#define DRIVER_NAME    "rpbridge"
#define DRIVER_AUTHOR  "BAUER GROUP <kb@de.bauer-group.com>"
#define DRIVER_DESC    "RPBridge USB multi-bus bridge — RPBP v1 transport"

static const struct usb_device_id rpbridge_id_table[] = {
	{ USB_DEVICE(RPBRIDGE_USB_VID, RPBRIDGE_USB_PID) },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(usb, rpbridge_id_table);

/* ───── Refcount accessors ───────────────────────────────────────── */

static void rpbridge_dev_release(struct kref *kref)
{
	struct rpbridge_dev *dev = container_of(kref, struct rpbridge_dev,
						kref);

	/*
	 * Final cleanup. By the time we land here:
	 *   - usb_set_intfdata(NULL) has been called (disconnect path).
	 *   - All aux children have been unregistered, so no callback
	 *     can race with us.
	 *   - transport_lock / seq_lock have no waiters (otherwise the
	 *     refcount would still be elevated).
	 */
	mutex_destroy(&dev->transport_lock);
	mutex_destroy(&dev->seq_lock);
	mutex_destroy(&dev->aux_lock);

	kfree(dev->caps);
	dev->caps = NULL;

	if (dev->udev) {
		usb_put_dev(dev->udev);
		dev->udev = NULL;
	}

	kfree(dev);
}

struct rpbridge_dev *rpbridge_dev_get(struct rpbridge_dev *dev)
{
	if (dev)
		kref_get(&dev->kref);
	return dev;
}

void rpbridge_dev_put(struct rpbridge_dev *dev)
{
	if (dev)
		kref_put(&dev->kref, rpbridge_dev_release);
}

/* ───── Endpoint discovery ───────────────────────────────────────── */

/*
 * Locate the RPBP bulk-IN / bulk-OUT pair on the bound interface.
 * The vendor interface is currently the only one that exposes a
 * matching pair; refining this to honour an interface protocol
 * field would be a hardening step before publication.
 */
static int rpbridge_find_bulk_pair(struct usb_interface *intf,
				   struct rpbridge_dev *dev)
{
	struct usb_host_interface *alt;
	int i;

	if (!intf || !dev)
		return -EINVAL;

	alt = intf->cur_altsetting;
	if (!alt)
		return -ENODEV;

	for (i = 0; i < alt->desc.bNumEndpoints; ++i) {
		struct usb_endpoint_descriptor *ep =
			&alt->endpoint[i].desc;

		if (!usb_endpoint_xfer_bulk(ep))
			continue;
		if (usb_endpoint_dir_in(ep) && !dev->bulk_in_addr) {
			dev->bulk_in_addr = ep->bEndpointAddress;
			dev->bulk_in_size = usb_endpoint_maxp(ep);
		} else if (usb_endpoint_dir_out(ep) && !dev->bulk_out_addr) {
			dev->bulk_out_addr = ep->bEndpointAddress;
			dev->bulk_out_size = usb_endpoint_maxp(ep);
		}
	}

	if (!dev->bulk_in_addr || !dev->bulk_out_addr)
		return -ENODEV;
	return 0;
}

/* ───── Probe / disconnect ───────────────────────────────────────── */

static int rpbridge_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	struct rpbridge_dev *dev;
	int ret;

	(void)id;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->kref);
	mutex_init(&dev->transport_lock);
	mutex_init(&dev->seq_lock);
	mutex_init(&dev->aux_lock);
	INIT_LIST_HEAD(&dev->aux_children);

	dev->udev = usb_get_dev(interface_to_usbdev(intf));
	if (!dev->udev) {
		ret = -ENODEV;
		goto err_free_locks;
	}
	dev->intf = intf;

	ret = rpbridge_find_bulk_pair(intf, dev);
	if (ret) {
		dev_err(&intf->dev,
			"rpbridge: no RPBP bulk pair on interface %u: %d\n",
			intf->cur_altsetting->desc.bInterfaceNumber, ret);
		goto err_put_udev;
	}

	/* Stash before HELLO so a hostile init can find us back even
	 * if the helper bails halfway. The disconnect path nulls
	 * intfdata before tearing the kref down, so the only effect
	 * of an early stash is that disconnect() correctly releases
	 * the partially-initialised state. */
	usb_set_intfdata(intf, dev);

	dev_info(&intf->dev,
		 "rpbridge: probed (bulk IN=0x%02x OUT=0x%02x, "
		 "in_max=%d out_max=%d)\n",
		 dev->bulk_in_addr, dev->bulk_out_addr,
		 dev->bulk_in_size, dev->bulk_out_size);

	ret = rpbridge_session_hello(dev);
	if (ret) {
		dev_err(&intf->dev, "rpbridge: HELLO failed: %d\n", ret);
		goto err_clear_intfdata;
	}

	ret = rpbridge_session_fetch_caps(dev);
	if (ret) {
		dev_err(&intf->dev,
			"rpbridge: capability fetch failed: %d\n", ret);
		goto err_clear_intfdata;
	}

	dev_info(&intf->dev,
		 "rpbridge: firmware='%s' board='%s' protocol=%u.%u.%u "
		 "(buses advertised: %zu)\n",
		 dev->caps->fw[0]    ? dev->caps->fw    : "?",
		 dev->caps->board[0] ? dev->caps->board : "?",
		 dev->caps->proto[0], dev->caps->proto[1], dev->caps->proto[2],
		 dev->caps->bus_count);

	ret = rpbridge_spawn_aux_children(dev);
	if (ret) {
		dev_err(&intf->dev,
			"rpbridge: aux child spawn failed: %d\n", ret);
		goto err_clear_intfdata;
	}

	return 0;

err_clear_intfdata:
	/* After usb_set_intfdata() any latent caller could in principle
	 * have grabbed a reference. Drop our initial kref and let the
	 * release callback be the single chokepoint that frees state +
	 * usb_put_dev()s the parent — no double-free, no leaks. */
	usb_set_intfdata(intf, NULL);
	rpbridge_dev_put(dev);
	return ret;

err_put_udev:
	usb_put_dev(dev->udev);
	dev->udev = NULL;
err_free_locks:
	mutex_destroy(&dev->transport_lock);
	mutex_destroy(&dev->seq_lock);
	mutex_destroy(&dev->aux_lock);
	kfree(dev);
	return ret;
}

static void rpbridge_disconnect(struct usb_interface *intf)
{
	struct rpbridge_dev *dev = usb_get_intfdata(intf);

	if (!dev)
		return;

	/*
	 * Block any latent (in-flight) callbacks from finding the
	 * driver via usb_get_intfdata() before we start tearing the
	 * state down. After this point new callers see NULL and bail.
	 */
	usb_set_intfdata(intf, NULL);

	dev_info(&intf->dev, "rpbridge: disconnected\n");

	/*
	 * Unregister aux children FIRST. Each child's release fn
	 * calls rpbridge_dev_put() — which only frees state when
	 * the last reference drops. Children hold a kref each via
	 * spawn_one(), so the parent kref drops below cannot race
	 * with a still-active subsystem callback.
	 */
	rpbridge_unregister_aux_children(dev);

	rpbridge_dev_put(dev);
}

/* ───── Module glue ──────────────────────────────────────────────── */

static struct usb_driver rpbridge_driver = {
	.name        = DRIVER_NAME,
	.id_table    = rpbridge_id_table,
	.probe       = rpbridge_probe,
	.disconnect  = rpbridge_disconnect,
	/*
	 * No suspend/resume yet — the firmware does not implement
	 * runtime PM, so the kernel's default (assume the device
	 * stays awake) is the right behaviour.
	 */
};

/*
 * Subsystem auxiliary_drivers register on module-load and unregister
 * on module-exit. Registration order matters only insofar as cleanup
 * runs in reverse: if rpbridge_iio_register() fails we must already
 * have cleaned up gpio/i2c/spi/pwm.
 */
static int __init rpbridge_module_init(void)
{
	int ret;

	ret = rpbridge_gpio_register();   if (ret) goto err;
	ret = rpbridge_i2c_register();    if (ret) goto err_gpio;
	ret = rpbridge_spi_register();    if (ret) goto err_i2c;
	ret = rpbridge_pwm_register();    if (ret) goto err_spi;
	ret = rpbridge_iio_register();    if (ret) goto err_pwm;
	ret = usb_register(&rpbridge_driver);
	if (ret)
		goto err_iio;
	return 0;

err_iio:  rpbridge_iio_unregister();
err_pwm:  rpbridge_pwm_unregister();
err_spi:  rpbridge_spi_unregister();
err_i2c:  rpbridge_i2c_unregister();
err_gpio: rpbridge_gpio_unregister();
err:
	return ret;
}

static void __exit rpbridge_module_exit(void)
{
	usb_deregister(&rpbridge_driver);
	rpbridge_iio_unregister();
	rpbridge_pwm_unregister();
	rpbridge_spi_unregister();
	rpbridge_i2c_unregister();
	rpbridge_gpio_unregister();
}

module_init(rpbridge_module_init);
module_exit(rpbridge_module_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1.0");
