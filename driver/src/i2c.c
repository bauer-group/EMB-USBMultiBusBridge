// SPDX-License-Identifier: GPL-2.0
/*
 * I²C subsystem — auxiliary_driver that registers an i2c_adapter
 * backed by the firmware's I²C subsystem (RPBP 0x01).
 *
 * Wire layer mirrors firmware/src/commands/cmd_i2c.c:
 *
 *   XFER       in  : bus:u8 addr:u8 flags:u8 tx_len:u16le rx_len:u16le
 *                    tx_data[tx_len]
 *              out : rx_data[rx_len]                    (on RPBP_OK)
 *   SET_FREQ   in  : bus:u8 freq_hz:u32le
 *              out : (none)
 *   GET_FREQ   in  : bus:u8
 *              out : freq_hz:u32le
 *
 * The bus index inside the firmware = the auxdev->id (which the
 * cap parser fills from buses[i].idx). Each adapter binds exactly
 * one firmware bus so a board with two I²C controllers shows up as
 * two /dev/i2c-N devices.
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "auxbus.h"
#include "rpbp.h"
#include "rpbridge.h"
#include "transport.h"

/* RPBP_MAX_PAYLOAD_LEN (4096) minus the per-XFER header (7 bytes
 * plus the 3-byte CMD_REQUEST prelude that transport.c prepends)
 * gives us a strict upper bound on a single I²C transfer's tx+rx
 * total. We round down a touch to leave headroom for any future
 * flag bytes. */
#define RPBRIDGE_I2C_MAX_LEN  4032u

struct rpbridge_i2c {
	struct rpbridge_aux_dev *aux;
	struct i2c_adapter       adapter;
};

static int i2c_fw_xfer(struct rpbridge_dev *parent,
		       u8 bus, u8 addr, u8 flags,
		       const u8 *tx, u16 tx_len,
		       u8 *rx, u16 rx_len, size_t *rx_actual)
{
	u8  *args   = NULL;
	size_t args_len;
	int  ret;

	if (tx_len > RPBRIDGE_I2C_MAX_LEN || rx_len > RPBRIDGE_I2C_MAX_LEN)
		return -EMSGSIZE;
	if (tx_len > 0 && !tx)
		return -EINVAL;
	if (rx_len > 0 && !rx)
		return -EINVAL;

	args_len = 7u + tx_len;
	args = kmalloc(args_len, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	args[0] = bus;
	args[1] = addr;
	args[2] = flags;
	args[3] = (u8)(tx_len & 0xFF);
	args[4] = (u8)((tx_len >> 8) & 0xFF);
	args[5] = (u8)(rx_len & 0xFF);
	args[6] = (u8)((rx_len >> 8) & 0xFF);
	if (tx_len)
		memcpy(&args[7], tx, tx_len);

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_I2C, RPBP_I2C_XFER,
				  args, args_len,
				  rx, rx_len, rx_actual);
	kfree(args);
	return ret;
}

static int rpbridge_i2c_xfer(struct i2c_adapter *adap,
			     struct i2c_msg *msgs, int num)
{
	struct rpbridge_i2c *i = i2c_get_adapdata(adap);
	int idx;

	if (!i || !i->aux || !i->aux->parent)
		return -ENODEV;

	/*
	 * One RPBP XFER per i2c_msg. Linux's i2c-core can pass
	 * multiple messages per master_xfer call (typically a
	 * 'write register, then read' pair); we walk them in order.
	 * The firmware's I2C_FUNC_NOSTART path is not yet exposed,
	 * so each message issues a STOP at the end. That works for
	 * 99 % of devices but breaks SMBus block-process; tracking
	 * that as a future enhancement.
	 */
	for (idx = 0; idx < num; ++idx) {
		struct i2c_msg *m = &msgs[idx];
		u8  flags = 0;
		size_t actual = 0;
		int ret;

		if (m->flags & I2C_M_TEN)
			return -EOPNOTSUPP;
		if (m->len > RPBRIDGE_I2C_MAX_LEN)
			return -EMSGSIZE;

		if (m->flags & I2C_M_RD) {
			ret = i2c_fw_xfer(i->aux->parent, (u8)i->aux->bus_idx,
					  (u8)m->addr, flags,
					  NULL, 0, m->buf, m->len, &actual);
			if (ret)
				return ret;
			if (actual != m->len)
				return -EIO;
		} else {
			ret = i2c_fw_xfer(i->aux->parent, (u8)i->aux->bus_idx,
					  (u8)m->addr, flags,
					  m->buf, m->len, NULL, 0, &actual);
			if (ret)
				return ret;
		}
	}
	return num;
}

static u32 rpbridge_i2c_func(struct i2c_adapter *adap)
{
	(void)adap;
	/* Plain master + 7-bit addressing only. SMBus quirks are
	 * emulated by i2c-core when needed. */
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm rpbridge_i2c_algo = {
	.master_xfer    = rpbridge_i2c_xfer,
	.functionality  = rpbridge_i2c_func,
};

static int rpbridge_i2c_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);
	struct rpbridge_i2c *i;
	int ret;

	(void)id;

	i = devm_kzalloc(&auxdev->dev, sizeof(*i), GFP_KERNEL);
	if (!i)
		return -ENOMEM;

	i->aux = aux;

	i->adapter.owner       = THIS_MODULE;
	/* I2C_CLASS_HWMON is the only adapter-class hint that
	 * survived into 7.0 untouched (I2C_CLASS_SPD was retired
	 * with the SPD-i2c subsystem migration). */
	i->adapter.class       = I2C_CLASS_HWMON;
	i->adapter.algo        = &rpbridge_i2c_algo;
	i->adapter.dev.parent  = &auxdev->dev;
	scnprintf(i->adapter.name, sizeof(i->adapter.name),
		  "rpbridge-i2c%u", aux->bus_idx);
	i2c_set_adapdata(&i->adapter, i);

	ret = devm_i2c_add_adapter(&auxdev->dev, &i->adapter);
	if (ret)
		return ret;

	auxiliary_set_drvdata(auxdev, i);
	dev_info(&auxdev->dev,
		 "rpbridge.i2c: registered i2c-%d (firmware bus=%u)\n",
		 i->adapter.nr, aux->bus_idx);
	return 0;
}

static void rpbridge_i2c_remove(struct auxiliary_device *auxdev)
{
	(void)auxdev; /* devm_i2c_add_adapter unwinds automatically */
}

static const struct auxiliary_device_id rpbridge_i2c_id_table[] = {
	{ .name = "rpbridge.i2c" },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(auxiliary, rpbridge_i2c_id_table);

static struct auxiliary_driver rpbridge_i2c_aux_driver = {
	.name        = "i2c",
	.id_table    = rpbridge_i2c_id_table,
	.probe       = rpbridge_i2c_probe,
	.remove      = rpbridge_i2c_remove,
};

int rpbridge_i2c_register(void)
{
	return auxiliary_driver_register(&rpbridge_i2c_aux_driver);
}

void rpbridge_i2c_unregister(void)
{
	auxiliary_driver_unregister(&rpbridge_i2c_aux_driver);
}
