/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RPBridge — driver-private per-device state and the small set of
 * symbols shared between the USB driver entry (main.c), the
 * transport layer (transport.c), and (later) the auxiliary_bus
 * subsystem children.
 */

#ifndef RPBRIDGE_RPBRIDGE_H
#define RPBRIDGE_RPBRIDGE_H

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/usb.h>

struct rpbp_caps;     /* forward — defined in cap.h */

/**
 * struct rpbridge_dev - per-USB-interface state.
 *
 * @kref:        refcount; released only after the last consumer
 *               (USB core's disconnect, or a still-running aux child
 *               callback) calls rpbridge_dev_put().
 * @udev:        owning USB device — strong reference held via
 *               usb_get_dev() in probe; released by usb_put_dev() in
 *               the kref release callback.
 * @intf:        the bound interface. Pointer is valid until the
 *               disconnect() callback returns.
 * @bulk_in_addr:  RPBP bulk IN endpoint address (with direction bit).
 * @bulk_out_addr: RPBP bulk OUT endpoint address.
 * @bulk_in_size:  wMaxPacketSize for the IN endpoint (sized for one
 *                 round-trip recv buffer allocation).
 * @bulk_out_size: wMaxPacketSize for the OUT endpoint.
 * @transport_lock: serialises every round_trip() — only one request
 *                  in flight per device on the control channel. Held
 *                  across both bulk-OUT and bulk-IN, so any user of
 *                  the lock MUST be in process context.
 * @seq_lock:    serialises the rolling-seq allocator for the
 *               control channel. Held only briefly; transport_lock
 *               is the long-held one.
 * @next_seq:    next 16-bit sequence value for the control channel.
 * @caps:        capability map decoded at probe time. Owned here;
 *               freed in the kref release.
 */
struct rpbridge_dev {
	struct kref         kref;

	struct usb_device  *udev;
	struct usb_interface *intf;

	__u8                bulk_in_addr;
	__u8                bulk_out_addr;
	int                 bulk_in_size;
	int                 bulk_out_size;

	struct mutex        transport_lock;
	struct mutex        seq_lock;
	u16                 next_seq;

	struct rpbp_caps   *caps;
};

/* Refcount accessors. Defined in main.c so the release callback
 * sees the full struct definitions of all linked-in subsystems. */
struct rpbridge_dev *rpbridge_dev_get(struct rpbridge_dev *dev);
void rpbridge_dev_put(struct rpbridge_dev *dev);

#endif /* RPBRIDGE_RPBRIDGE_H */
