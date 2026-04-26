// SPDX-License-Identifier: GPL-2.0
/*
 * DMX-512 transmitter — auxiliary_driver that exposes the firmware's
 * DMX subsystem (RPBP 0x08) as a misc character device. Linux has no
 * standard DMX subsystem; the convention from OLA / QLC+ / etc. is a
 * char-device with write() into a 513-byte universe buffer plus
 * ioctls for start/stop/break-timing.
 *
 * Wire layer mirrors firmware/src/commands/cmd_dmx.c:
 *
 *   GET_INFO   in  (none)
 *              out count:u8 pin:u8 running:u8 reserved:u8
 *                  slot_count:u16le break_us:u16le mab_us:u16le
 *   SET_SLOTS  in  idx:u8 offset:u16le bytes[…]
 *   START      in  idx:u8
 *   STOP       in  idx:u8
 *   SET_BREAK  in  idx:u8 break_us:u16le mab_us:u16le
 *
 * Userspace surface:
 *
 *   /dev/rpbridge-dmx<bus_idx>          character device
 *   write(fd, buf, n)                   write n slots starting at the
 *                                       current file offset
 *   lseek(fd, off, SEEK_SET)            reposition (offset 0 = DMX
 *                                       start-code byte)
 *   ioctl(RPBRIDGE_DMX_IOC_INFO, &info) get current state
 *   ioctl(RPBRIDGE_DMX_IOC_START)
 *   ioctl(RPBRIDGE_DMX_IOC_STOP)
 *   ioctl(RPBRIDGE_DMX_IOC_SET_BREAK, &t)
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

/* DMX-512 universe = 513 bytes (start-code + 512 slots). */
#define RPBRIDGE_DMX_UNIVERSE_LEN  513u

#define RPBRIDGE_DMX_MAX_CHUNK     4090u

struct rpbridge_dmx_info_uapi {
	__u8  count;
	__u8  pin;
	__u8  running;
	__u8  reserved;
	__u16 slot_count;
	__u16 break_us;
	__u16 mab_us;
} __packed;

struct rpbridge_dmx_break_uapi {
	__u8  idx;
	__u8  reserved;
	__u16 break_us;
	__u16 mab_us;
} __packed;

#define RPBRIDGE_DMX_IOC_MAGIC      'D'
#define RPBRIDGE_DMX_IOC_INFO       _IOR(RPBRIDGE_DMX_IOC_MAGIC, 1, \
					  struct rpbridge_dmx_info_uapi)
#define RPBRIDGE_DMX_IOC_START      _IO (RPBRIDGE_DMX_IOC_MAGIC, 2)
#define RPBRIDGE_DMX_IOC_STOP       _IO (RPBRIDGE_DMX_IOC_MAGIC, 3)
#define RPBRIDGE_DMX_IOC_SET_BREAK  _IOW(RPBRIDGE_DMX_IOC_MAGIC, 4, \
					  struct rpbridge_dmx_break_uapi)

struct rpbridge_dmx {
	struct rpbridge_aux_dev *aux;
	struct miscdevice        misc;
	char                     dev_name[24];
	struct mutex             lock;
};

/* ───── Wire helpers ─────────────────────────────────────────────── */

static int dmx_fw_get_info(struct rpbridge_dev *parent,
			   struct rpbridge_dmx_info_uapi *out)
{
	u8 reply[10];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_DMX, RPBP_DMX_GET_INFO,
				  NULL, 0,
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != sizeof(reply))
		return -EPROTO;

	out->count      = reply[0];
	out->pin        = reply[1];
	out->running    = reply[2];
	out->reserved   = 0;
	out->slot_count = (u16)reply[4] | ((u16)reply[5] << 8);
	out->break_us   = (u16)reply[6] | ((u16)reply[7] << 8);
	out->mab_us     = (u16)reply[8] | ((u16)reply[9] << 8);
	return 0;
}

static int dmx_fw_set_slots(struct rpbridge_dev *parent, u8 idx,
			    u16 offset, const u8 *data, u16 data_len)
{
	u8 *args = NULL;
	size_t reply_len = 0;
	u8 dummy;
	int ret;

	if (!data || data_len == 0 || data_len > RPBRIDGE_DMX_MAX_CHUNK)
		return -EINVAL;

	args = kmalloc(3u + data_len, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	args[0] = idx;
	args[1] = (u8)(offset & 0xFF);
	args[2] = (u8)((offset >> 8) & 0xFF);
	memcpy(&args[3], data, data_len);

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_DMX, RPBP_DMX_SET_SLOTS,
				  args, 3u + data_len,
				  &dummy, 0, &reply_len);
	kfree(args);
	return ret;
}

static int dmx_fw_start_stop(struct rpbridge_dev *parent, u8 idx, bool start)
{
	u8 args[1] = { idx };
	size_t reply_len = 0;
	u8 dummy;

	return rpbridge_round_trip(parent, RPBP_SUBSYS_DMX,
				   start ? RPBP_DMX_START : RPBP_DMX_STOP,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int dmx_fw_set_break(struct rpbridge_dev *parent, u8 idx,
			    u16 break_us, u16 mab_us)
{
	u8 args[5];
	size_t reply_len = 0;
	u8 dummy;

	args[0] = idx;
	args[1] = (u8)(break_us & 0xFF);
	args[2] = (u8)((break_us >> 8) & 0xFF);
	args[3] = (u8)(mab_us & 0xFF);
	args[4] = (u8)((mab_us >> 8) & 0xFF);

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_DMX, RPBP_DMX_SET_BREAK,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

/* ───── file_operations ──────────────────────────────────────────── */

static struct rpbridge_dmx *file_to_dmx(struct file *f)
{
	struct miscdevice *m = f->private_data;

	return container_of(m, struct rpbridge_dmx, misc);
}

static ssize_t rpbridge_dmx_write(struct file *f, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct rpbridge_dmx *d = file_to_dmx(f);
	u8 *kbuf = NULL;
	loff_t pos;
	size_t written = 0;
	int ret;

	if (!d || !d->aux || !d->aux->parent)
		return -ENODEV;
	if (count == 0)
		return 0;

	pos = *ppos;
	if (pos < 0 || pos >= RPBRIDGE_DMX_UNIVERSE_LEN)
		return -EINVAL;
	if (count > (size_t)(RPBRIDGE_DMX_UNIVERSE_LEN - pos))
		count = RPBRIDGE_DMX_UNIVERSE_LEN - (size_t)pos;

	kbuf = kmalloc(min_t(size_t, count, RPBRIDGE_DMX_MAX_CHUNK),
		       GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	mutex_lock(&d->lock);
	while (written < count) {
		size_t chunk = min_t(size_t, count - written,
				     RPBRIDGE_DMX_MAX_CHUNK);

		if (copy_from_user(kbuf, ubuf + written, chunk)) {
			ret = -EFAULT;
			goto out;
		}
		ret = dmx_fw_set_slots(d->aux->parent,
				       (u8)d->aux->bus_idx,
				       (u16)(pos + written),
				       kbuf, (u16)chunk);
		if (ret)
			goto out;
		written += chunk;
	}
	*ppos = pos + written;
	ret = (int)written;

out:
	mutex_unlock(&d->lock);
	kfree(kbuf);
	return ret;
}

static loff_t rpbridge_dmx_llseek(struct file *f, loff_t off, int whence)
{
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:  newpos = off; break;
	case SEEK_CUR:  newpos = f->f_pos + off; break;
	case SEEK_END:  newpos = RPBRIDGE_DMX_UNIVERSE_LEN + off; break;
	default:        return -EINVAL;
	}
	if (newpos < 0 || newpos > RPBRIDGE_DMX_UNIVERSE_LEN)
		return -EINVAL;
	f->f_pos = newpos;
	return newpos;
}

static long rpbridge_dmx_ioctl(struct file *f, unsigned int cmd,
			       unsigned long arg)
{
	struct rpbridge_dmx *d = file_to_dmx(f);
	int ret;

	if (!d || !d->aux || !d->aux->parent)
		return -ENODEV;

	switch (cmd) {
	case RPBRIDGE_DMX_IOC_INFO: {
		struct rpbridge_dmx_info_uapi info = { 0 };

		mutex_lock(&d->lock);
		ret = dmx_fw_get_info(d->aux->parent, &info);
		mutex_unlock(&d->lock);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case RPBRIDGE_DMX_IOC_START:
		mutex_lock(&d->lock);
		ret = dmx_fw_start_stop(d->aux->parent, (u8)d->aux->bus_idx,
					true);
		mutex_unlock(&d->lock);
		return ret;
	case RPBRIDGE_DMX_IOC_STOP:
		mutex_lock(&d->lock);
		ret = dmx_fw_start_stop(d->aux->parent, (u8)d->aux->bus_idx,
					false);
		mutex_unlock(&d->lock);
		return ret;
	case RPBRIDGE_DMX_IOC_SET_BREAK: {
		struct rpbridge_dmx_break_uapi t;

		if (copy_from_user(&t, (void __user *)arg, sizeof(t)))
			return -EFAULT;
		if (t.idx != d->aux->bus_idx)
			return -EINVAL;
		mutex_lock(&d->lock);
		ret = dmx_fw_set_break(d->aux->parent, t.idx,
				       t.break_us, t.mab_us);
		mutex_unlock(&d->lock);
		return ret;
	}
	}
	return -ENOTTY;
}

static const struct file_operations rpbridge_dmx_fops = {
	.owner          = THIS_MODULE,
	.write          = rpbridge_dmx_write,
	.llseek         = rpbridge_dmx_llseek,
	.unlocked_ioctl = rpbridge_dmx_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ───── auxiliary_driver glue ────────────────────────────────────── */

static int rpbridge_dmx_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);
	struct rpbridge_dmx *d;
	int ret;

	(void)id;

	d = devm_kzalloc(&auxdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->aux = aux;
	mutex_init(&d->lock);

	scnprintf(d->dev_name, sizeof(d->dev_name),
		  "rpbridge-dmx%u", aux->bus_idx);
	d->misc.minor   = MISC_DYNAMIC_MINOR;
	d->misc.name    = d->dev_name;
	d->misc.fops    = &rpbridge_dmx_fops;
	d->misc.parent  = &auxdev->dev;

	ret = misc_register(&d->misc);
	if (ret) {
		mutex_destroy(&d->lock);
		return ret;
	}

	auxiliary_set_drvdata(auxdev, d);
	dev_info(&auxdev->dev,
		 "rpbridge.dmx: registered /dev/%s (firmware bus=%u)\n",
		 d->dev_name, aux->bus_idx);
	return 0;
}

static void rpbridge_dmx_remove(struct auxiliary_device *auxdev)
{
	struct rpbridge_dmx *d = auxiliary_get_drvdata(auxdev);

	if (!d)
		return;
	misc_deregister(&d->misc);
	mutex_destroy(&d->lock);
}

static const struct auxiliary_device_id rpbridge_dmx_id_table[] = {
	{ .name = "rpbridge.dmx" },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(auxiliary, rpbridge_dmx_id_table);

static struct auxiliary_driver rpbridge_dmx_aux_driver = {
	.name        = "dmx",
	.id_table    = rpbridge_dmx_id_table,
	.probe       = rpbridge_dmx_probe,
	.remove      = rpbridge_dmx_remove,
};

int rpbridge_dmx_register(void)
{
	return auxiliary_driver_register(&rpbridge_dmx_aux_driver);
}

void rpbridge_dmx_unregister(void)
{
	auxiliary_driver_unregister(&rpbridge_dmx_aux_driver);
}
