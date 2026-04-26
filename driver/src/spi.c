// SPDX-License-Identifier: GPL-2.0
/*
 * SPI subsystem — auxiliary_driver that registers an spi_controller
 * backed by the firmware's SPI subsystem (RPBP 0x02).
 *
 * Wire layer mirrors firmware/src/commands/cmd_spi.c:
 *
 *   XFER       in  : instance:u8 cs_pin:u8 flags:u8 reserved:u8
 *                    tx_len:u16le rx_len:u16le tx_data[tx_len]
 *              out : rx_data[rx_len]
 *   SET_MODE   in  : instance:u8 mode_bits:u8
 *              out : (none)
 *   SET_FREQ   in  : instance:u8 freq_hz:u32le
 *              out : (none)
 *   GET_FREQ   in  : instance:u8
 *              out : freq_hz:u32le
 *   CS_ASSERT  in  : instance:u8 cs_pin:u8
 *              out : (none)
 *   CS_RELEASE in  : instance:u8 cs_pin:u8
 *              out : (none)
 *
 * The cs_pin field is 0xFF when the kernel layer drives CS itself
 * (gpiolib mode) — that's the path Linux takes for spidev.
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/string.h>

#include "auxbus.h"
#include "rpbp.h"
#include "rpbridge.h"
#include "transport.h"

#define RPBRIDGE_SPI_MAX_LEN  4032u

/* No firmware-side CS manipulation — the kernel's gpiolib drives
 * the CS line if /dev/spidev needs it. The firmware ignores the
 * cs_pin argument when the manual-CS flag is clear, so 0xFF here
 * is purely a wire-level "ignore this byte" marker. */
#define RPBRIDGE_SPI_CS_NONE  0xFFu

struct rpbridge_spi {
	struct rpbridge_aux_dev *aux;
	struct spi_controller   *ctlr;
	u32                      freq_hz;     /* last applied; 0 = unset */
	u8                       mode_bits;   /* last applied */
};

static int spi_fw_set_mode(struct rpbridge_dev *parent,
			   u8 instance, u8 mode_bits)
{
	u8 args[2] = { instance, mode_bits };
	size_t reply_len = 0;
	u8 dummy;

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_SPI, RPBP_SPI_SET_MODE,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int spi_fw_set_freq(struct rpbridge_dev *parent,
			   u8 instance, u32 freq_hz)
{
	u8 args[5];
	size_t reply_len = 0;
	u8 dummy;

	args[0] = instance;
	args[1] = (u8)(freq_hz & 0xFF);
	args[2] = (u8)((freq_hz >> 8)  & 0xFF);
	args[3] = (u8)((freq_hz >> 16) & 0xFF);
	args[4] = (u8)((freq_hz >> 24) & 0xFF);

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_SPI, RPBP_SPI_SET_FREQ,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int spi_fw_xfer(struct rpbridge_dev *parent, u8 instance,
		       const u8 *tx, u8 *rx, u16 len)
{
	u8 *args = NULL;
	size_t args_len, rx_len = 0;
	int ret;

	if (len > RPBRIDGE_SPI_MAX_LEN)
		return -EMSGSIZE;
	if (len > 0 && (!tx || !rx))
		return -EINVAL;

	args_len = 8u + (size_t)len;
	args = kmalloc(args_len, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	args[0] = instance;
	args[1] = RPBRIDGE_SPI_CS_NONE;
	args[2] = 0;                /* flags: no manual CS */
	args[3] = 0;                /* reserved */
	args[4] = (u8)(len & 0xFF);
	args[5] = (u8)((len >> 8) & 0xFF);
	args[6] = (u8)(len & 0xFF);
	args[7] = (u8)((len >> 8) & 0xFF);
	if (len)
		memcpy(&args[8], tx, len);

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_SPI, RPBP_SPI_XFER,
				  args, args_len,
				  rx, len, &rx_len);
	kfree(args);
	if (ret)
		return ret;
	if (rx_len != len)
		return -EIO;
	return 0;
}

static int rpbridge_spi_setup(struct spi_device *spi)
{
	struct spi_controller *ctlr = spi->controller;
	struct rpbridge_spi *s = spi_controller_get_devdata(ctlr);
	u8 mode_bits = 0;
	int ret;

	if (!s || !s->aux || !s->aux->parent)
		return -ENODEV;

	if (spi->mode & SPI_CPHA)
		mode_bits |= RPBP_SPI_MODE_CPHA;
	if (spi->mode & SPI_CPOL)
		mode_bits |= RPBP_SPI_MODE_CPOL;
	if (spi->mode & SPI_LSB_FIRST)
		mode_bits |= RPBP_SPI_MODE_LSB_FIRST;

	if (mode_bits != s->mode_bits) {
		ret = spi_fw_set_mode(s->aux->parent, (u8)s->aux->bus_idx,
				      mode_bits);
		if (ret)
			return ret;
		s->mode_bits = mode_bits;
	}

	if (spi->max_speed_hz && spi->max_speed_hz != s->freq_hz) {
		ret = spi_fw_set_freq(s->aux->parent, (u8)s->aux->bus_idx,
				      spi->max_speed_hz);
		if (ret)
			return ret;
		s->freq_hz = spi->max_speed_hz;
	}
	return 0;
}

static int rpbridge_spi_transfer_one(struct spi_controller *ctlr,
				     struct spi_device *spi,
				     struct spi_transfer *xfer)
{
	struct rpbridge_spi *s = spi_controller_get_devdata(ctlr);
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	int ret;

	if (!s || !s->aux || !s->aux->parent)
		return -ENODEV;
	if (xfer->len > RPBRIDGE_SPI_MAX_LEN)
		return -EMSGSIZE;
	if (!xfer->len)
		return 0;

	/* The firmware always echoes len bytes back regardless of
	 * direction (full-duplex shift). Allocate a dummy on the side
	 * for unidirectional transfers so the wire helper has buffers
	 * of equal length. */
	if (!tx) {
		void *zero = devm_kzalloc(&ctlr->dev, xfer->len, GFP_KERNEL);
		/* devm tracks until ctlr unwinds — fine for transient
		 * pre-allocations on the master_xfer path; spi-core
		 * sequences calls so we won't pile these up. */
		if (!zero)
			return -ENOMEM;
		tx = zero;
	}
	if (!rx) {
		void *sink = devm_kzalloc(&ctlr->dev, xfer->len, GFP_KERNEL);

		if (!sink)
			return -ENOMEM;
		rx = sink;
	}

	ret = spi_fw_xfer(s->aux->parent, (u8)s->aux->bus_idx,
			  tx, rx, xfer->len);
	if (ret)
		return ret;

	spi_finalize_current_transfer(ctlr);
	return 0;
}

static int rpbridge_spi_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);
	struct spi_controller *ctlr;
	struct rpbridge_spi *s;
	int ret;

	(void)id;

	ctlr = devm_spi_alloc_host(&auxdev->dev, sizeof(*s));
	if (!ctlr)
		return -ENOMEM;

	s = spi_controller_get_devdata(ctlr);
	s->aux  = aux;
	s->ctlr = ctlr;

	ctlr->bus_num            = -1;
	ctlr->mode_bits          = SPI_CPOL | SPI_CPHA | SPI_LSB_FIRST;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->min_speed_hz       = 1000;
	ctlr->max_speed_hz       = 50 * 1000 * 1000;
	ctlr->setup              = rpbridge_spi_setup;
	ctlr->transfer_one       = rpbridge_spi_transfer_one;
	/* SPI-core handles CS via gpiolib by default in 7.0; we don't
	 * advertise SPI_CONTROLLER_HALF_DUPLEX — the firmware always
	 * shifts both directions on the wire. */

	ret = devm_spi_register_controller(&auxdev->dev, ctlr);
	if (ret)
		return ret;

	auxiliary_set_drvdata(auxdev, s);
	dev_info(&auxdev->dev,
		 "rpbridge.spi: registered spi%d (firmware bus=%u)\n",
		 ctlr->bus_num, aux->bus_idx);
	return 0;
}

static void rpbridge_spi_remove(struct auxiliary_device *auxdev)
{
	(void)auxdev;
}

static const struct auxiliary_device_id rpbridge_spi_id_table[] = {
	{ .name = "rpbridge.spi" },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(auxiliary, rpbridge_spi_id_table);

static struct auxiliary_driver rpbridge_spi_aux_driver = {
	.name        = "spi",
	.id_table    = rpbridge_spi_id_table,
	.probe       = rpbridge_spi_probe,
	.remove      = rpbridge_spi_remove,
};

int rpbridge_spi_register(void)
{
	return auxiliary_driver_register(&rpbridge_spi_aux_driver);
}

void rpbridge_spi_unregister(void)
{
	auxiliary_driver_unregister(&rpbridge_spi_aux_driver);
}
