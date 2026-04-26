// SPDX-License-Identifier: GPL-2.0
/*
 * Session init — HELLO exchange + capability fetch. Called once
 * during probe() before any subsystem children get spawned.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb.h>

#include "cap.h"
#include "rpbp.h"
#include "rpbp_framing.h"
#include "rpbridge.h"
#include "session.h"
#include "transport.h"

/*
 * HELLO is a top-level message type rather than a CMD_REQUEST, so it
 * does not go through rpbridge_round_trip() — that helper is hard-
 * wired to the CMD_REQUEST/CMD_RESPONSE pair on the control channel.
 * We assemble the frame inline here, send it, read the reply, and
 * verify it is indeed a HELLO with matching major version.
 */
int rpbridge_session_hello(struct rpbridge_dev *dev)
{
	struct rpbp_header out_hdr = { 0 };
	struct rpbp_header in_hdr;
	u8 *out_buf = NULL;
	u8 *in_buf  = NULL;
	size_t out_len = 0;
	int actual = 0;
	int ret;
	u16 seq;
	const u8 hello_payload[3] = {
		RPBP_VERSION,  /* major  */
		0,             /* minor  */
		0,             /* patch  */
	};

	if (!dev)
		return -EINVAL;

	out_buf = kmalloc(RPBP_MAX_FRAME_LEN, GFP_KERNEL);
	in_buf  = kmalloc(RPBP_MAX_FRAME_LEN, GFP_KERNEL);
	if (!out_buf || !in_buf) {
		ret = -ENOMEM;
		goto out_free;
	}

	mutex_lock(&dev->seq_lock);
	seq = dev->next_seq++;
	mutex_unlock(&dev->seq_lock);

	out_hdr.msg_type     = RPBP_MSG_HELLO;
	out_hdr.flags        = 0;
	out_hdr.channel      = RPBP_CHAN_CONTROL;
	out_hdr.seq          = seq;
	out_hdr.timestamp_us = 0;

	ret = rpbp_frame_build(&out_hdr,
			       hello_payload, sizeof(hello_payload),
			       out_buf, RPBP_MAX_FRAME_LEN, &out_len);
	if (ret)
		goto out_free;

	mutex_lock(&dev->transport_lock);

	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, dev->bulk_out_addr),
			   out_buf, (int)out_len, &actual,
			   RPBRIDGE_RT_TIMEOUT_MS);
	if (ret) {
		dev_err(&dev->intf->dev,
			"rpbridge: HELLO OUT failed: %d\n", ret);
		goto out_unlock;
	}
	if (actual != (int)out_len) {
		ret = -EIO;
		goto out_unlock;
	}

	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, dev->bulk_in_addr),
			   in_buf, RPBP_MAX_FRAME_LEN, &actual,
			   RPBRIDGE_RT_TIMEOUT_MS);
	if (ret) {
		dev_err(&dev->intf->dev,
			"rpbridge: HELLO IN failed: %d\n", ret);
		goto out_unlock;
	}

	mutex_unlock(&dev->transport_lock);

	if (actual <= 0) {
		ret = -EIO;
		goto out_free;
	}

	ret = rpbp_frame_validate(in_buf, (size_t)actual, &in_hdr);
	if (ret) {
		dev_err(&dev->intf->dev,
			"rpbridge: HELLO reply malformed: %d\n", ret);
		goto out_free;
	}
	if (in_hdr.msg_type != RPBP_MSG_HELLO) {
		dev_err(&dev->intf->dev,
			"rpbridge: HELLO reply has wrong msg_type 0x%02x\n",
			in_hdr.msg_type);
		ret = -EPROTO;
		goto out_free;
	}
	if (in_hdr.payload_len >= 1) {
		const u8 *body = rpbp_frame_payload(in_buf, &in_hdr);

		if (body && body[0] != RPBP_VERSION) {
			dev_err(&dev->intf->dev,
				"rpbridge: firmware speaks RPBP v%u, "
				"driver is v%u — refusing to attach\n",
				body[0], RPBP_VERSION);
			ret = -EPROTONOSUPPORT;
			goto out_free;
		}
	}
	ret = 0;

out_free:
	kfree(out_buf);
	kfree(in_buf);
	return ret;

out_unlock:
	mutex_unlock(&dev->transport_lock);
	kfree(out_buf);
	kfree(in_buf);
	return ret;
}

int rpbridge_session_fetch_caps(struct rpbridge_dev *dev)
{
	struct rpbp_caps *caps = NULL;
	u8 *cbor_buf = NULL;
	size_t cbor_len = 0;
	int ret;

	if (!dev)
		return -EINVAL;
	if (dev->caps) {
		/* Idempotent: refuse to reparse if already populated.
		 * probe() must call this exactly once. */
		return -EEXIST;
	}

	cbor_buf = kmalloc(RPBP_MAX_PAYLOAD_LEN, GFP_KERNEL);
	caps     = kzalloc(sizeof(*caps), GFP_KERNEL);
	if (!cbor_buf || !caps) {
		ret = -ENOMEM;
		goto out_free;
	}

	ret = rpbridge_round_trip(dev,
				  RPBP_SUBSYS_SYS, RPBP_SYS_GET_CAPABILITIES,
				  NULL, 0,
				  cbor_buf, RPBP_MAX_PAYLOAD_LEN, &cbor_len);
	if (ret) {
		dev_err(&dev->intf->dev,
			"rpbridge: GET_CAPABILITIES failed: %d\n", ret);
		goto out_free;
	}

	ret = rpbp_caps_parse(cbor_buf, cbor_len, caps);
	if (ret) {
		dev_err(&dev->intf->dev,
			"rpbridge: capability map parse failed: %d\n",
			ret);
		goto out_free;
	}

	/* Hand ownership over to dev — kref release will free it. */
	dev->caps = caps;
	caps = NULL;
	ret = 0;

out_free:
	kfree(cbor_buf);
	kfree(caps);
	return ret;
}
