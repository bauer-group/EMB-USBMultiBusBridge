// SPDX-License-Identifier: GPL-2.0
/*
 * I²S TX subsystem — auxiliary_driver that exposes the firmware's
 * I²S transmitter (RPBP 0x0A) as a misc character device.
 *
 * Why misc-cdev and not ASoC: a full ASoC machine + codec + DAI link
 * stack is heavy. The firmware's I²S surface is intentionally
 * minimal (rate + start/stop + raw PCM write); a misc-cdev with
 * write() of stereo PCM frames matches that scope. A future
 * commit can layer ASoC on top — the wire layer below stays
 * unchanged.
 *
 * Wire layer mirrors firmware/src/commands/cmd_i2s.c:
 *
 *   GET_INFO   in  (none)
 *              out 12 bytes (count/bclk/lrclk/dout/sample_rate:u32le/
 *                            bit_depth/running/reserved×2)
 *   CONFIGURE  in  idx:u8 sample_rate:u32le
 *   START      in  idx:u8
 *   STOP       in  idx:u8
 *   WRITE      in  idx:u8 pcm[…]              (multiple of 4 bytes
 *                                              for 16-bit stereo)
 *
 * Userspace surface:
 *
 *   /dev/rpbridge-i2s<bus_idx>          character device
 *   write(fd, frame_data, n)            stream PCM frames
 *   ioctl(RPBRIDGE_I2S_IOC_INFO)
 *   ioctl(RPBRIDGE_I2S_IOC_CONFIGURE, &cfg)
 *   ioctl(RPBRIDGE_I2S_IOC_START)
 *   ioctl(RPBRIDGE_I2S_IOC_STOP)
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "auxbus.h"
#include "rpbp.h"
#include "rpbridge.h"
#include "transport.h"

#define RPBRIDGE_I2S_MAX_CHUNK   4090u

/* Stereo 16-bit frame = 4 bytes; alignment requirement for write(). */
#define RPBRIDGE_I2S_FRAME_LEN   4u

struct rpbridge_i2s_info_uapi {
	__u8  count;
	__u8  bclk_pin;
	__u8  lrclk_pin;
	__u8  dout_pin;
	__u32 sample_rate;
	__u8  bit_depth;
	__u8  running;
	__u8  reserved[2];
} __packed;

struct rpbridge_i2s_configure_uapi {
	__u8  idx;
	__u8  reserved[3];
	__u32 sample_rate;
} __packed;

#define RPBRIDGE_I2S_IOC_MAGIC      'I'
#define RPBRIDGE_I2S_IOC_INFO       _IOR(RPBRIDGE_I2S_IOC_MAGIC, 1, \
					  struct rpbridge_i2s_info_uapi)
#define RPBRIDGE_I2S_IOC_CONFIGURE  _IOW(RPBRIDGE_I2S_IOC_MAGIC, 2, \
					  struct rpbridge_i2s_configure_uapi)
#define RPBRIDGE_I2S_IOC_START      _IO (RPBRIDGE_I2S_IOC_MAGIC, 3)
#define RPBRIDGE_I2S_IOC_STOP       _IO (RPBRIDGE_I2S_IOC_MAGIC, 4)

struct rpbridge_i2s {
	struct rpbridge_aux_dev *aux;
	struct miscdevice        misc;
	char                     dev_name[24];
	struct mutex             lock;
};

static int i2s_fw_get_info(struct rpbridge_dev *parent,
			   struct rpbridge_i2s_info_uapi *out)
{
	u8 reply[12];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_I2S, RPBP_I2S_GET_INFO,
				  NULL, 0,
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != sizeof(reply))
		return -EPROTO;

	out->count       = reply[0];
	out->bclk_pin    = reply[1];
	out->lrclk_pin   = reply[2];
	out->dout_pin    = reply[3];
	out->sample_rate = (u32)reply[4] | ((u32)reply[5] << 8)
			 | ((u32)reply[6] << 16) | ((u32)reply[7] << 24);
	out->bit_depth   = reply[8];
	out->running     = reply[9];
	out->reserved[0] = 0;
	out->reserved[1] = 0;
	return 0;
}

static int i2s_fw_configure(struct rpbridge_dev *parent, u8 idx,
			    u32 sample_rate)
{
	u8 args[5];
	size_t reply_len = 0;
	u8 dummy;

	args[0] = idx;
	args[1] = (u8)(sample_rate & 0xFF);
	args[2] = (u8)((sample_rate >> 8) & 0xFF);
	args[3] = (u8)((sample_rate >> 16) & 0xFF);
	args[4] = (u8)((sample_rate >> 24) & 0xFF);

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_I2S, RPBP_I2S_CONFIGURE,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int i2s_fw_start_stop(struct rpbridge_dev *parent, u8 idx, bool start)
{
	u8 args[1] = { idx };
	size_t reply_len = 0;
	u8 dummy;

	return rpbridge_round_trip(parent, RPBP_SUBSYS_I2S,
				   start ? RPBP_I2S_START : RPBP_I2S_STOP,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int i2s_fw_write(struct rpbridge_dev *parent, u8 idx,
			const u8 *pcm, size_t len)
{
	u8 *args = NULL;
	size_t reply_len = 0;
	u8 dummy;
	int ret;

	if (!pcm || len == 0 || len > RPBRIDGE_I2S_MAX_CHUNK)
		return -EINVAL;

	args = kmalloc(1u + len, GFP_KERNEL);
	if (!args)
		return -ENOMEM;
	args[0] = idx;
	memcpy(&args[1], pcm, len);

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_I2S, RPBP_I2S_WRITE,
				  args, 1u + len,
				  &dummy, 0, &reply_len);
	kfree(args);
	return ret;
}

/* ───── file_operations ──────────────────────────────────────────── */

static struct rpbridge_i2s *file_to_i2s(struct file *f)
{
	struct miscdevice *m = f->private_data;

	return container_of(m, struct rpbridge_i2s, misc);
}

static ssize_t rpbridge_i2s_write(struct file *f, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct rpbridge_i2s *s = file_to_i2s(f);
	u8 *kbuf = NULL;
	size_t written = 0;
	int ret;

	(void)ppos;

	if (!s || !s->aux || !s->aux->parent)
		return -ENODEV;
	if (count == 0)
		return 0;

	/* PCM streaming requires frame alignment. We still accept
	 * partial frames at the head — the firmware buffers — but
	 * a write that ends mid-frame is almost always a userspace
	 * bug and surfaces here as -EINVAL so it gets caught in
	 * tests rather than silently produces buzz on the wire. */
	if (count % RPBRIDGE_I2S_FRAME_LEN)
		return -EINVAL;

	kbuf = kmalloc(min_t(size_t, count, RPBRIDGE_I2S_MAX_CHUNK),
		       GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	mutex_lock(&s->lock);
	while (written < count) {
		size_t chunk = min_t(size_t, count - written,
				     RPBRIDGE_I2S_MAX_CHUNK);

		/* Round chunk down to a frame boundary so each
		 * RPBP WRITE call also lands on a frame edge. */
		chunk -= chunk % RPBRIDGE_I2S_FRAME_LEN;
		if (!chunk)
			break;

		if (copy_from_user(kbuf, ubuf + written, chunk)) {
			ret = -EFAULT;
			goto out;
		}
		ret = i2s_fw_write(s->aux->parent, (u8)s->aux->bus_idx,
				   kbuf, chunk);
		if (ret)
			goto out;
		written += chunk;
	}
	ret = (int)written;

out:
	mutex_unlock(&s->lock);
	kfree(kbuf);
	return ret;
}

static long rpbridge_i2s_ioctl(struct file *f, unsigned int cmd,
			       unsigned long arg)
{
	struct rpbridge_i2s *s = file_to_i2s(f);
	int ret;

	if (!s || !s->aux || !s->aux->parent)
		return -ENODEV;

	switch (cmd) {
	case RPBRIDGE_I2S_IOC_INFO: {
		struct rpbridge_i2s_info_uapi info = { 0 };

		mutex_lock(&s->lock);
		ret = i2s_fw_get_info(s->aux->parent, &info);
		mutex_unlock(&s->lock);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case RPBRIDGE_I2S_IOC_CONFIGURE: {
		struct rpbridge_i2s_configure_uapi cfg;

		if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg)))
			return -EFAULT;
		if (cfg.idx != s->aux->bus_idx)
			return -EINVAL;
		mutex_lock(&s->lock);
		ret = i2s_fw_configure(s->aux->parent, cfg.idx,
				       cfg.sample_rate);
		mutex_unlock(&s->lock);
		return ret;
	}
	case RPBRIDGE_I2S_IOC_START:
		mutex_lock(&s->lock);
		ret = i2s_fw_start_stop(s->aux->parent,
					(u8)s->aux->bus_idx, true);
		mutex_unlock(&s->lock);
		return ret;
	case RPBRIDGE_I2S_IOC_STOP:
		mutex_lock(&s->lock);
		ret = i2s_fw_start_stop(s->aux->parent,
					(u8)s->aux->bus_idx, false);
		mutex_unlock(&s->lock);
		return ret;
	}
	return -ENOTTY;
}

static const struct file_operations rpbridge_i2s_fops = {
	.owner          = THIS_MODULE,
	.write          = rpbridge_i2s_write,
	.unlocked_ioctl = rpbridge_i2s_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

static int rpbridge_i2s_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);
	struct rpbridge_i2s *s;
	int ret;

	(void)id;

	s = devm_kzalloc(&auxdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->aux = aux;
	mutex_init(&s->lock);

	scnprintf(s->dev_name, sizeof(s->dev_name),
		  "rpbridge-i2s%u", aux->bus_idx);
	s->misc.minor   = MISC_DYNAMIC_MINOR;
	s->misc.name    = s->dev_name;
	s->misc.fops    = &rpbridge_i2s_fops;
	s->misc.parent  = &auxdev->dev;

	ret = misc_register(&s->misc);
	if (ret) {
		mutex_destroy(&s->lock);
		return ret;
	}

	auxiliary_set_drvdata(auxdev, s);
	dev_info(&auxdev->dev,
		 "rpbridge.i2s: registered /dev/%s (firmware bus=%u)\n",
		 s->dev_name, aux->bus_idx);
	return 0;
}

static void rpbridge_i2s_remove(struct auxiliary_device *auxdev)
{
	struct rpbridge_i2s *s = auxiliary_get_drvdata(auxdev);

	if (!s)
		return;
	misc_deregister(&s->misc);
	mutex_destroy(&s->lock);
}

static const struct auxiliary_device_id rpbridge_i2s_id_table[] = {
	{ .name = "rpbridge.i2s" },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(auxiliary, rpbridge_i2s_id_table);

static struct auxiliary_driver rpbridge_i2s_aux_driver = {
	.name        = "i2s",
	.id_table    = rpbridge_i2s_id_table,
	.probe       = rpbridge_i2s_probe,
	.remove      = rpbridge_i2s_remove,
};

int rpbridge_i2s_register(void)
{
	return auxiliary_driver_register(&rpbridge_i2s_aux_driver);
}

void rpbridge_i2s_unregister(void)
{
	auxiliary_driver_unregister(&rpbridge_i2s_aux_driver);
}
