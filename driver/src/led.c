// SPDX-License-Identifier: GPL-2.0
/*
 * LED-strip subsystem — auxiliary_driver that exposes the firmware's
 * addressable LED output (RPBP 0x07) as a misc character device.
 *
 * Why misc-cdev and not the kernel <linux/leds.h> "multicolor" class:
 * the multicolor class is sized for a single multi-color status LED
 * (R+G+B brightness, possibly W). Addressable strips with N=30..300
 * pixels do not map onto it ergonomically. The convention in
 * userspace tooling (OpenRGB, Hyperion, custom daemons) is a
 * write()-able byte stream with seek + ioctl(GET_INFO) — that's what
 * we ship here.
 *
 * Wire layer mirrors firmware/src/commands/cmd_led.c:
 *
 *   GET_INFO   in  (none)
 *              out count:u8 pin:u8 format:u8 reserved:u8
 *                  pixel_count:u16le max_pixels:u16le
 *   CONFIGURE  in  idx:u8 format:u8 pixel_count:u16le
 *   WRITE      in  idx:u8 offset:u16le pixel_data[…]
 *   CLEAR      in  idx:u8
 *
 * Userspace surface:
 *
 *   /dev/rpbridge-led<bus_idx>           character device
 *   write(fd, buf, n)                    splatter bytes at the
 *                                        current offset (firmware
 *                                        format-aware: GRB/RGB/GRBW)
 *   lseek(fd, off, SEEK_SET)             reposition
 *   ioctl(fd, RPBRIDGE_LED_IOC_INFO, &info)
 *   ioctl(fd, RPBRIDGE_LED_IOC_CLEAR)
 *   ioctl(fd, RPBRIDGE_LED_IOC_CONFIGURE, &cfg)
 *
 * Bytes-per-pixel is implied by the format byte returned from
 * GET_INFO: GRB/RGB = 3 bytes, GRBW = 4 bytes. Userspace is expected
 * to honour that — we do not convert internally.
 */

#include <linux/auxiliary_bus.h>
#include <linux/cdev.h>
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

/* Firmware caps each WRITE at one RPBP frame. RPBP_MAX_PAYLOAD_LEN
 * = 4096; the WRITE prelude (idx + offset) is 3 bytes; the
 * CMD_REQUEST prelude prepended by the transport layer is another
 * 3 bytes. So the largest single write() call we can pass through
 * in one round-trip is 4090. We loop in chunks for anything larger. */
#define RPBRIDGE_LED_MAX_CHUNK   4090u

struct rpbridge_led_info_uapi {
	__u8  count;
	__u8  pin;
	__u8  format;
	__u8  reserved;
	__u16 pixel_count;
	__u16 max_pixels;
} __packed;

struct rpbridge_led_configure_uapi {
	__u8  idx;
	__u8  format;
	__u16 pixel_count;
} __packed;

#define RPBRIDGE_LED_IOC_MAGIC      'L'
#define RPBRIDGE_LED_IOC_INFO       _IOR(RPBRIDGE_LED_IOC_MAGIC, 1, \
					  struct rpbridge_led_info_uapi)
#define RPBRIDGE_LED_IOC_CLEAR      _IO (RPBRIDGE_LED_IOC_MAGIC, 2)
#define RPBRIDGE_LED_IOC_CONFIGURE  _IOW(RPBRIDGE_LED_IOC_MAGIC, 3, \
					  struct rpbridge_led_configure_uapi)

struct rpbridge_led {
	struct rpbridge_aux_dev *aux;
	struct miscdevice        misc;
	char                     dev_name[24];   /* "rpbridge-ledN" */
	struct mutex             lock;
};

/* ───── Wire helpers ─────────────────────────────────────────────── */

static int led_fw_get_info(struct rpbridge_dev *parent,
			   struct rpbridge_led_info_uapi *out)
{
	u8 reply[8];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_LED, RPBP_LED_GET_INFO,
				  NULL, 0,
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != sizeof(reply))
		return -EPROTO;

	out->count       = reply[0];
	out->pin         = reply[1];
	out->format      = reply[2];
	out->reserved    = 0;
	out->pixel_count = (u16)reply[4] | ((u16)reply[5] << 8);
	out->max_pixels  = (u16)reply[6] | ((u16)reply[7] << 8);
	return 0;
}

static int led_fw_configure(struct rpbridge_dev *parent,
			    u8 idx, u8 format, u16 pixel_count)
{
	u8 args[4];
	size_t reply_len = 0;
	u8 dummy;

	args[0] = idx;
	args[1] = format;
	args[2] = (u8)(pixel_count & 0xFF);
	args[3] = (u8)((pixel_count >> 8) & 0xFF);

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_LED, RPBP_LED_CONFIGURE,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int led_fw_write_chunk(struct rpbridge_dev *parent, u8 idx,
			      u16 offset, const u8 *data, u16 data_len)
{
	u8 *args = NULL;
	size_t reply_len = 0;
	u8 dummy;
	int ret;

	if (!data || data_len == 0 || data_len > RPBRIDGE_LED_MAX_CHUNK)
		return -EINVAL;

	args = kmalloc(3u + data_len, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	args[0] = idx;
	args[1] = (u8)(offset & 0xFF);
	args[2] = (u8)((offset >> 8) & 0xFF);
	memcpy(&args[3], data, data_len);

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_LED, RPBP_LED_WRITE,
				  args, 3u + data_len,
				  &dummy, 0, &reply_len);
	kfree(args);
	return ret;
}

static int led_fw_clear(struct rpbridge_dev *parent, u8 idx)
{
	u8 args[1] = { idx };
	size_t reply_len = 0;
	u8 dummy;

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_LED, RPBP_LED_CLEAR,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

/* ───── file_operations ──────────────────────────────────────────── */

static struct rpbridge_led *file_to_led(struct file *f)
{
	struct miscdevice *m = f->private_data;

	return container_of(m, struct rpbridge_led, misc);
}

static ssize_t rpbridge_led_write(struct file *f, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct rpbridge_led *l = file_to_led(f);
	u8 *kbuf = NULL;
	loff_t pos;
	size_t written = 0;
	int ret;

	if (!l || !l->aux || !l->aux->parent)
		return -ENODEV;
	if (count == 0)
		return 0;

	pos = *ppos;
	if (pos < 0 || pos > 0xFFFF)
		return -EINVAL;
	if (count > 0x10000u - (size_t)pos)
		return -EFBIG;

	kbuf = kmalloc(min_t(size_t, count, RPBRIDGE_LED_MAX_CHUNK),
		       GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	mutex_lock(&l->lock);
	while (written < count) {
		size_t chunk = min_t(size_t, count - written,
				     RPBRIDGE_LED_MAX_CHUNK);

		if (copy_from_user(kbuf, ubuf + written, chunk)) {
			ret = -EFAULT;
			goto out;
		}
		ret = led_fw_write_chunk(l->aux->parent,
					 (u8)l->aux->bus_idx,
					 (u16)(pos + written),
					 kbuf, (u16)chunk);
		if (ret)
			goto out;
		written += chunk;
	}
	*ppos = pos + written;
	ret = (int)written;

out:
	mutex_unlock(&l->lock);
	kfree(kbuf);
	return ret;
}

static loff_t rpbridge_led_llseek(struct file *f, loff_t off, int whence)
{
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:  newpos = off; break;
	case SEEK_CUR:  newpos = f->f_pos + off; break;
	default:        return -EINVAL;
	}
	if (newpos < 0 || newpos > 0xFFFF)
		return -EINVAL;
	f->f_pos = newpos;
	return newpos;
}

static long rpbridge_led_ioctl(struct file *f, unsigned int cmd,
			       unsigned long arg)
{
	struct rpbridge_led *l = file_to_led(f);
	int ret;

	if (!l || !l->aux || !l->aux->parent)
		return -ENODEV;

	switch (cmd) {
	case RPBRIDGE_LED_IOC_INFO: {
		struct rpbridge_led_info_uapi info = { 0 };

		mutex_lock(&l->lock);
		ret = led_fw_get_info(l->aux->parent, &info);
		mutex_unlock(&l->lock);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case RPBRIDGE_LED_IOC_CLEAR: {
		mutex_lock(&l->lock);
		ret = led_fw_clear(l->aux->parent, (u8)l->aux->bus_idx);
		mutex_unlock(&l->lock);
		return ret;
	}
	case RPBRIDGE_LED_IOC_CONFIGURE: {
		struct rpbridge_led_configure_uapi cfg;

		if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg)))
			return -EFAULT;
		if (cfg.idx != l->aux->bus_idx)
			return -EINVAL;
		mutex_lock(&l->lock);
		ret = led_fw_configure(l->aux->parent,
				       cfg.idx, cfg.format, cfg.pixel_count);
		mutex_unlock(&l->lock);
		return ret;
	}
	}
	return -ENOTTY;
}

static const struct file_operations rpbridge_led_fops = {
	.owner          = THIS_MODULE,
	.write          = rpbridge_led_write,
	.llseek         = rpbridge_led_llseek,
	.unlocked_ioctl = rpbridge_led_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ───── auxiliary_driver glue ────────────────────────────────────── */

static int rpbridge_led_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);
	struct rpbridge_led *l;
	int ret;

	(void)id;

	l = devm_kzalloc(&auxdev->dev, sizeof(*l), GFP_KERNEL);
	if (!l)
		return -ENOMEM;

	l->aux = aux;
	mutex_init(&l->lock);

	scnprintf(l->dev_name, sizeof(l->dev_name),
		  "rpbridge-led%u", aux->bus_idx);
	l->misc.minor   = MISC_DYNAMIC_MINOR;
	l->misc.name    = l->dev_name;
	l->misc.fops    = &rpbridge_led_fops;
	l->misc.parent  = &auxdev->dev;

	ret = misc_register(&l->misc);
	if (ret) {
		mutex_destroy(&l->lock);
		return ret;
	}

	auxiliary_set_drvdata(auxdev, l);
	dev_info(&auxdev->dev,
		 "rpbridge.led: registered /dev/%s (firmware bus=%u)\n",
		 l->dev_name, aux->bus_idx);
	return 0;
}

static void rpbridge_led_remove(struct auxiliary_device *auxdev)
{
	struct rpbridge_led *l = auxiliary_get_drvdata(auxdev);

	if (!l)
		return;
	misc_deregister(&l->misc);
	mutex_destroy(&l->lock);
}

static const struct auxiliary_device_id rpbridge_led_id_table[] = {
	{ .name = "rpbridge.led" },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(auxiliary, rpbridge_led_id_table);

static struct auxiliary_driver rpbridge_led_aux_driver = {
	.name        = "led",
	.id_table    = rpbridge_led_id_table,
	.probe       = rpbridge_led_probe,
	.remove      = rpbridge_led_remove,
};

int rpbridge_led_register(void)
{
	return auxiliary_driver_register(&rpbridge_led_aux_driver);
}

void rpbridge_led_unregister(void)
{
	auxiliary_driver_unregister(&rpbridge_led_aux_driver);
}
