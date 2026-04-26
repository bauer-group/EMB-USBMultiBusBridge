// SPDX-License-Identifier: GPL-2.0
/*
 * RPBP transport — synchronous CMD_REQUEST/CMD_RESPONSE pair over the
 * bulk IN/OUT endpoints of the vendor interface.
 *
 * Locking discipline:
 *   - transport_lock is held across the OUT and IN bulk transfers so
 *     a second thread can't interleave a request mid-RX. usb_bulk_msg
 *     may sleep; so does the lock; both are process-context only.
 *   - seq_lock is held briefly to allocate the next sequence value.
 *
 * Memory discipline:
 *   - Allocates two buffers per call: out_buf (frame to send) and
 *     in_buf (frame to receive). Both kfree()d before return on every
 *     path (success, error, or timeout). No leaks on -ETIMEDOUT or
 *     -ENOMEM.
 *   - Frame buffers sized to %RPBP_MAX_FRAME_LEN — the spec ceiling.
 *     A 4 KiB request + 4 KiB reply per call is acceptable
 *     considering at most one in-flight per device.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb.h>

#include "rpbp.h"
#include "rpbp_framing.h"
#include "rpbridge.h"
#include "transport.h"

/*
 * Reserve 3 bytes of CMD_REQUEST/CMD_RESPONSE header (subsys, opcode,
 * status). Defined as a constant so call sites don't sprinkle magic
 * numbers.
 */
#define CMD_HDR_LEN  3u

/* Allocate a u16 sequence value for the control channel.
 * Wrapping is safe — the firmware's CompletionTable doesn't care
 * about absolute values, only matching against the value it received. */
static u16 take_seq(struct rpbridge_dev *dev)
{
	u16 v;

	mutex_lock(&dev->seq_lock);
	v = dev->next_seq++;
	mutex_unlock(&dev->seq_lock);
	return v;
}

int rpbridge_round_trip(struct rpbridge_dev *dev,
			enum rpbp_subsys subsys, u8 opcode,
			const u8 *args, size_t args_len,
			u8 *reply, size_t reply_capacity, size_t *reply_len)
{
	struct rpbp_header out_hdr = { 0 };
	struct rpbp_header in_hdr;
	u8 *out_buf = NULL;
	u8 *in_buf = NULL;
	u8 *req_payload = NULL;
	size_t out_len = 0;
	size_t req_payload_len;
	int actual = 0;
	int ret;
	u16 seq;

	if (!dev || !reply_len)
		return -EINVAL;
	if (args_len > 0 && !args)
		return -EINVAL;
	if (reply_capacity > 0 && !reply)
		return -EINVAL;
	if (args_len > RPBP_MAX_PAYLOAD_LEN - CMD_HDR_LEN)
		return -EMSGSIZE;

	*reply_len = 0;

	out_buf = kmalloc(RPBP_MAX_FRAME_LEN, GFP_KERNEL);
	in_buf  = kmalloc(RPBP_MAX_FRAME_LEN, GFP_KERNEL);
	if (!out_buf || !in_buf) {
		ret = -ENOMEM;
		goto out_free;
	}

	/*
	 * Build the CMD_REQUEST payload: [subsys][opcode][args...].
	 * We assemble it inside out_buf at the eventual payload offset
	 * so rpbp_frame_build() can splice it without a second copy.
	 */
	req_payload_len = CMD_HDR_LEN + args_len;
	req_payload = kmalloc(req_payload_len, GFP_KERNEL);
	if (!req_payload) {
		ret = -ENOMEM;
		goto out_free;
	}
	req_payload[0] = (u8)subsys;
	req_payload[1] = opcode;
	req_payload[2] = 0; /* status placeholder, ignored on request */
	if (args_len)
		memcpy(&req_payload[CMD_HDR_LEN], args, args_len);

	seq = take_seq(dev);
	out_hdr.msg_type     = RPBP_MSG_CMD_REQUEST;
	out_hdr.flags        = 0;
	out_hdr.channel      = RPBP_CHAN_CONTROL;
	out_hdr.seq          = seq;
	out_hdr.timestamp_us = 0;

	ret = rpbp_frame_build(&out_hdr,
			       req_payload, req_payload_len,
			       out_buf, RPBP_MAX_FRAME_LEN, &out_len);
	if (ret)
		goto out_free;

	mutex_lock(&dev->transport_lock);

	/* OUT: ship the request. */
	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, dev->bulk_out_addr),
			   out_buf, (int)out_len, &actual,
			   RPBRIDGE_RT_TIMEOUT_MS);
	if (ret) {
		dev_dbg(&dev->intf->dev,
			"rpbridge: OUT bulk failed: %d (subsys=%u op=0x%02x)\n",
			ret, (unsigned)subsys, opcode);
		goto out_unlock;
	}
	if (actual != (int)out_len) {
		dev_warn(&dev->intf->dev,
			 "rpbridge: short OUT bulk: %d/%zu\n",
			 actual, out_len);
		ret = -EIO;
		goto out_unlock;
	}

	/* IN: read the response. */
	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, dev->bulk_in_addr),
			   in_buf, RPBP_MAX_FRAME_LEN, &actual,
			   RPBRIDGE_RT_TIMEOUT_MS);
	if (ret) {
		dev_dbg(&dev->intf->dev,
			"rpbridge: IN bulk failed: %d\n", ret);
		goto out_unlock;
	}
	if (actual <= 0) {
		ret = -EIO;
		goto out_unlock;
	}

	mutex_unlock(&dev->transport_lock);

	/* Parse + validate (CRC, header sanity) outside the lock. */
	ret = rpbp_frame_validate(in_buf, (size_t)actual, &in_hdr);
	if (ret)
		goto out_free;

	if (in_hdr.msg_type != RPBP_MSG_CMD_RESPONSE) {
		ret = -EPROTO;
		goto out_free;
	}
	if (in_hdr.channel != RPBP_CHAN_CONTROL || in_hdr.seq != seq) {
		ret = -EPROTO;
		goto out_free;
	}
	if (in_hdr.payload_len < CMD_HDR_LEN) {
		ret = -EPROTO;
		goto out_free;
	}

	{
		const u8 *body = rpbp_frame_payload(in_buf, &in_hdr);
		size_t body_len = in_hdr.payload_len;
		enum rpbp_status status;
		size_t result_len;

		if (!body || body_len < CMD_HDR_LEN) {
			ret = -EPROTO;
			goto out_free;
		}
		/* Echo back of the same subsys+opcode protects against
		 * accidental cross-talk if the firmware ever multiplexes
		 * channels in the future. */
		if (body[0] != (u8)subsys || body[1] != opcode) {
			ret = -EPROTO;
			goto out_free;
		}
		status = (enum rpbp_status)body[2];
		result_len = body_len - CMD_HDR_LEN;

		if (status != RPBP_OK) {
			ret = rpbp_status_to_errno(status);
			goto out_free;
		}
		if (result_len > reply_capacity) {
			ret = -EMSGSIZE;
			goto out_free;
		}
		if (result_len > 0)
			memcpy(reply, &body[CMD_HDR_LEN], result_len);
		*reply_len = result_len;
		ret = 0;
	}

out_free:
	kfree(out_buf);
	kfree(in_buf);
	kfree(req_payload);
	return ret;

out_unlock:
	mutex_unlock(&dev->transport_lock);
	kfree(out_buf);
	kfree(in_buf);
	kfree(req_payload);
	return ret;
}
EXPORT_SYMBOL_GPL(rpbridge_round_trip);
