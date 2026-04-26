// SPDX-License-Identifier: GPL-2.0
/*
 * 1-Wire (Maxim/Dallas) bus master — auxiliary_driver that registers
 * a w1_bus_master backed by the firmware's RPBP 0x09 subsystem.
 *
 * Wire layer mirrors firmware/src/commands/cmd_onewire.c:
 *
 *   GET_INFO   in  (none)
 *              out count:u8 pin:u8 speed:u8 reserved:u8
 *   RESET      in  idx:u8
 *              out present:u8     (1 = slave detected, 0 = none)
 *   WRITE      in  idx:u8 bytes[…]
 *   READ       in  idx:u8 len:u16le
 *              out bytes[len]
 *   TRIPLET    in  idx:u8 dir:u8
 *              out result:u8      (bit0=id_bit, bit1=cmp_bit, bit2=dir)
 *
 * Linux integration: registers as a w1_bus_master under /sys/bus/w1/.
 * The kernel's w1_int.c then probes attached slaves, surfaces them
 * as `<family>-<id>` devices, and the per-family slave drivers
 * (w1_therm for DS18B20, w1_ds2433 for EEPROM, …) bind automatically.
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/w1.h>

#include "auxbus.h"
#include "rpbp.h"
#include "rpbridge.h"
#include "transport.h"

/* Firmware ONEWIRE_READ_MAX is documented at 4080 bytes per call.
 * w1-core typically issues blocks far below that (DS18B20 scratchpad
 * = 9 bytes, EEPROM page = 32 bytes); the upper bound is just a
 * safety net. */
#define RPBRIDGE_ONEWIRE_MAX     4080u

struct rpbridge_onewire {
	struct rpbridge_aux_dev *aux;
	struct w1_bus_master     master;
};

static int onewire_fw_reset(struct rpbridge_dev *parent, u8 idx,
			    u8 *present_out)
{
	u8 args[1] = { idx };
	u8 reply[1];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_RESET,
				  args, sizeof(args),
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != 1)
		return -EPROTO;
	*present_out = reply[0] & 1;
	return 0;
}

static int onewire_fw_write(struct rpbridge_dev *parent, u8 idx,
			    const u8 *data, size_t len)
{
	u8 *args = NULL;
	size_t reply_len = 0;
	u8 dummy;
	int ret;

	if (len == 0)
		return 0;
	if (!data || len > RPBRIDGE_ONEWIRE_MAX)
		return -EINVAL;

	args = kmalloc(1u + len, GFP_KERNEL);
	if (!args)
		return -ENOMEM;
	args[0] = idx;
	memcpy(&args[1], data, len);

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_WRITE,
				  args, 1u + len,
				  &dummy, 0, &reply_len);
	kfree(args);
	return ret;
}

static int onewire_fw_read(struct rpbridge_dev *parent, u8 idx,
			   u8 *out, size_t len)
{
	u8 args[3];
	size_t reply_len = 0;
	int ret;

	if (len == 0 || len > RPBRIDGE_ONEWIRE_MAX || !out)
		return -EINVAL;

	args[0] = idx;
	args[1] = (u8)(len & 0xFF);
	args[2] = (u8)((len >> 8) & 0xFF);

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_READ,
				  args, sizeof(args),
				  out, len, &reply_len);
	if (ret)
		return ret;
	if (reply_len != len)
		return -EPROTO;
	return 0;
}

static int onewire_fw_triplet(struct rpbridge_dev *parent, u8 idx,
			      u8 dir, u8 *result_out)
{
	u8 args[2] = { idx, dir };
	u8 reply[1];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_ONEWIRE, RPBP_ONEWIRE_TRIPLET,
				  args, sizeof(args),
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != 1)
		return -EPROTO;
	*result_out = reply[0];
	return 0;
}

/* ───── w1_bus_master callbacks ──────────────────────────────────── */

static struct rpbridge_onewire *to_rpow(void *data)
{
	return (struct rpbridge_onewire *)data;
}

static u8 rpbridge_w1_reset(void *data)
{
	struct rpbridge_onewire *o = to_rpow(data);
	u8 present = 0;

	if (!o || !o->aux || !o->aux->parent)
		return 1; /* "no presence" — safest fallback */
	if (onewire_fw_reset(o->aux->parent, (u8)o->aux->bus_idx, &present))
		return 1;
	/* w1-core convention: 0 = device present, 1 = no presence.
	 * Our firmware returns 1 = present, so invert at the boundary. */
	return present ? 0 : 1;
}

static u8 rpbridge_w1_read_block(void *data, u8 *buf, int len)
{
	struct rpbridge_onewire *o = to_rpow(data);

	if (!o || !o->aux || !o->aux->parent || !buf || len <= 0)
		return 0;
	if (onewire_fw_read(o->aux->parent, (u8)o->aux->bus_idx,
			    buf, (size_t)len))
		return 0;
	return (u8)len;
}

static void rpbridge_w1_write_block(void *data, const u8 *buf, int len)
{
	struct rpbridge_onewire *o = to_rpow(data);

	if (!o || !o->aux || !o->aux->parent || !buf || len <= 0)
		return;
	(void)onewire_fw_write(o->aux->parent, (u8)o->aux->bus_idx,
			       buf, (size_t)len);
}

static u8 rpbridge_w1_triplet(void *data, u8 dir)
{
	struct rpbridge_onewire *o = to_rpow(data);
	u8 result = 0;

	if (!o || !o->aux || !o->aux->parent)
		return 0;
	if (onewire_fw_triplet(o->aux->parent, (u8)o->aux->bus_idx,
			       dir, &result))
		return 0;
	return result;
}

/* ───── auxiliary_driver glue ────────────────────────────────────── */

static int rpbridge_onewire_probe(struct auxiliary_device *auxdev,
				  const struct auxiliary_device_id *id)
{
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);
	struct rpbridge_onewire *o;
	int ret;

	(void)id;

	o = devm_kzalloc(&auxdev->dev, sizeof(*o), GFP_KERNEL);
	if (!o)
		return -ENOMEM;

	o->aux = aux;
	o->master.data         = o;
	o->master.reset_bus    = rpbridge_w1_reset;
	o->master.read_block   = rpbridge_w1_read_block;
	o->master.write_block  = rpbridge_w1_write_block;
	o->master.triplet      = rpbridge_w1_triplet;

	ret = w1_add_master_device(&o->master);
	if (ret)
		return ret;

	auxiliary_set_drvdata(auxdev, o);
	dev_info(&auxdev->dev,
		 "rpbridge.onewire: registered w1 master (firmware bus=%u)\n",
		 aux->bus_idx);
	return 0;
}

static void rpbridge_onewire_remove(struct auxiliary_device *auxdev)
{
	struct rpbridge_onewire *o = auxiliary_get_drvdata(auxdev);

	if (!o)
		return;
	w1_remove_master_device(&o->master);
}

static const struct auxiliary_device_id rpbridge_onewire_id_table[] = {
	{ .name = "rpbridge.onewire" },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(auxiliary, rpbridge_onewire_id_table);

static struct auxiliary_driver rpbridge_onewire_aux_driver = {
	.name        = "onewire",
	.id_table    = rpbridge_onewire_id_table,
	.probe       = rpbridge_onewire_probe,
	.remove      = rpbridge_onewire_remove,
};

int rpbridge_onewire_register(void)
{
	return auxiliary_driver_register(&rpbridge_onewire_aux_driver);
}

void rpbridge_onewire_unregister(void)
{
	auxiliary_driver_unregister(&rpbridge_onewire_aux_driver);
}
