// SPDX-License-Identifier: GPL-2.0
/*
 * IIO/ADC subsystem — auxiliary_driver that registers an iio_dev
 * backed by the firmware's ADC subsystem (RPBP 0x05). Single-shot
 * reads only; triggered-buffer streaming lands when the async event
 * channel arrives (project M12).
 *
 * Wire layer mirrors firmware/src/commands/cmd_adc.c:
 *
 *   READ       in  channel:u8
 *              out raw:u16le mv:u16le
 *   GET_REF    in  (none)
 *              out vref_mv:u32le
 *   TEMP_READ  in  (none)
 *              out cdeg:s16le               (centi-degrees Celsius)
 *
 * Channel layout exposed to userspace:
 *
 *   in_voltage0_raw   .. in_voltage3_raw    (firmware channels 0..3)
 *   in_temp4_raw                            (firmware channel 4)
 *
 *   in_voltage_scale  computed from GET_REF as (vref_mv / 4096) so
 *                     `in_voltage*_raw * scale` yields millivolts.
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/iio/iio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "auxbus.h"
#include "rpbp.h"
#include "rpbridge.h"
#include "transport.h"

struct rpbridge_iio {
	struct rpbridge_aux_dev *aux;
	u32                      vref_mv;     /* cached at probe */
};

static int adc_fw_read(struct rpbridge_dev *parent, u8 channel,
		       u16 *raw_out, u16 *mv_out)
{
	u8 args[1] = { channel };
	u8 reply[4];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_ADC, RPBP_ADC_READ,
				  args, sizeof(args),
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != 4)
		return -EPROTO;

	*raw_out = (u16)reply[0] | ((u16)reply[1] << 8);
	*mv_out  = (u16)reply[2] | ((u16)reply[3] << 8);
	return 0;
}

static int adc_fw_get_ref(struct rpbridge_dev *parent, u32 *vref_mv_out)
{
	u8 reply[4];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_ADC, RPBP_ADC_GET_REF,
				  NULL, 0,
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != 4)
		return -EPROTO;
	*vref_mv_out = (u32)reply[0]
		     | ((u32)reply[1] << 8)
		     | ((u32)reply[2] << 16)
		     | ((u32)reply[3] << 24);
	return 0;
}

static int adc_fw_temp_read(struct rpbridge_dev *parent, s16 *cdeg_out)
{
	u8 reply[2];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_ADC, RPBP_ADC_TEMP_READ,
				  NULL, 0,
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != 2)
		return -EPROTO;
	*cdeg_out = (s16)((u16)reply[0] | ((u16)reply[1] << 8));
	return 0;
}

/* ───── iio_info callbacks ───────────────────────────────────────── */

static int rpbridge_iio_read_raw(struct iio_dev *idev,
				 struct iio_chan_spec const *chan,
				 int *val, int *val2, long mask)
{
	struct rpbridge_iio *i = iio_priv(idev);
	int ret;

	if (!i || !i->aux || !i->aux->parent)
		return -ENODEV;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->type == IIO_VOLTAGE) {
			u16 raw = 0, mv = 0;

			(void)mv;
			ret = adc_fw_read(i->aux->parent,
					  (u8)chan->channel, &raw, &mv);
			if (ret)
				return ret;
			*val = raw;
			return IIO_VAL_INT;
		}
		if (chan->type == IIO_TEMP) {
			s16 cdeg = 0;

			ret = adc_fw_temp_read(i->aux->parent, &cdeg);
			if (ret)
				return ret;
			/* Reported as centi-degrees Celsius; IIO temp
			 * convention is milli-degrees, so multiply by
			 * 10 at the boundary. */
			*val = (int)cdeg * 10;
			return IIO_VAL_INT;
		}
		return -EINVAL;

	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_VOLTAGE) {
			/* mV per LSB, expressed as int + 32-bit nano so
			 * IIO can give us 6 decimal places. The firmware
			 * is 12-bit, so step = vref_mv / 4096. */
			u64 nano = (u64)i->vref_mv * 1000000ULL;

			do_div(nano, 4096);
			*val  = (int)(nano / 1000);            /* mV */
			*val2 = (int)((nano % 1000) * 1000);   /* nanovolts */
			return IIO_VAL_INT_PLUS_NANO;
		}
		if (chan->type == IIO_TEMP) {
			/* Already milli-degrees-Celsius scale = 1. */
			*val  = 1;
			*val2 = 0;
			return IIO_VAL_INT;
		}
		return -EINVAL;
	}
	return -EINVAL;
}

static const struct iio_info rpbridge_iio_info = {
	.read_raw = rpbridge_iio_read_raw,
};

/*
 * Channel spec: 4 voltage channels (0..3) + one temperature channel.
 * The firmware's whitelist for external pins is GP26..GP29 →
 * channels 0..3, plus the on-die temperature sensor on channel 4.
 */
static const struct iio_chan_spec rpbridge_iio_channels[] = {
	{ .type = IIO_VOLTAGE, .indexed = 1, .channel = 0,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	  .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) },
	{ .type = IIO_VOLTAGE, .indexed = 1, .channel = 1,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	  .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) },
	{ .type = IIO_VOLTAGE, .indexed = 1, .channel = 2,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	  .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) },
	{ .type = IIO_VOLTAGE, .indexed = 1, .channel = 3,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	  .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) },
	{ .type = IIO_TEMP, .indexed = 1, .channel = 4,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_RAW)
			      | BIT(IIO_CHAN_INFO_SCALE) },
};

/* ───── auxiliary_driver glue ────────────────────────────────────── */

static int rpbridge_iio_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);
	struct iio_dev *idev;
	struct rpbridge_iio *i;
	int ret;

	(void)id;

	idev = devm_iio_device_alloc(&auxdev->dev, sizeof(*i));
	if (!idev)
		return -ENOMEM;

	i = iio_priv(idev);
	i->aux = aux;

	/* Best-effort cached vref so the SCALE info is honest from the
	 * first read. If GET_REF fails (firmware too old?) we fall back
	 * to the documented 3.3 V default. */
	if (adc_fw_get_ref(aux->parent, &i->vref_mv) || !i->vref_mv) {
		dev_info(&auxdev->dev,
			 "rpbridge.iio: GET_REF failed, defaulting to 3300 mV\n");
		i->vref_mv = 3300;
	}

	idev->name         = "rpbridge-iio";
	idev->info         = &rpbridge_iio_info;
	idev->modes        = INDIO_DIRECT_MODE;
	idev->channels     = rpbridge_iio_channels;
	idev->num_channels = ARRAY_SIZE(rpbridge_iio_channels);

	ret = devm_iio_device_register(&auxdev->dev, idev);
	if (ret)
		return ret;

	auxiliary_set_drvdata(auxdev, i);
	dev_info(&auxdev->dev,
		 "rpbridge.iio: registered iio_dev (vref=%u mV, %u channels)\n",
		 i->vref_mv,
		 (unsigned int)ARRAY_SIZE(rpbridge_iio_channels));
	return 0;
}

static void rpbridge_iio_remove(struct auxiliary_device *auxdev)
{
	(void)auxdev;
}

static const struct auxiliary_device_id rpbridge_iio_id_table[] = {
	{ .name = "rpbridge.adc" },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(auxiliary, rpbridge_iio_id_table);

static struct auxiliary_driver rpbridge_iio_aux_driver = {
	.name        = "iio",
	.id_table    = rpbridge_iio_id_table,
	.probe       = rpbridge_iio_probe,
	.remove      = rpbridge_iio_remove,
};

int rpbridge_iio_register(void)
{
	return auxiliary_driver_register(&rpbridge_iio_aux_driver);
}

void rpbridge_iio_unregister(void)
{
	auxiliary_driver_unregister(&rpbridge_iio_aux_driver);
}
