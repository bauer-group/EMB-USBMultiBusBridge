// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO subsystem — auxiliary_driver that registers a gpio_chip
 * backed by the firmware's GPIO subsystem (RPBP 0x03).
 *
 * Wire-layer encoding mirrors firmware/src/commands/cmd_gpio.c:
 *
 *   LIST       in  : (none)
 *              out : { count: u8, pins[count]: u8 }
 *   CONFIGURE  in  : pin: u8, dir: u8 (0=in, 1=out), pull: u8 (0/1/2)
 *              out : (none)
 *   SET        in  : pin: u8, value: u8
 *              out : (none)
 *   GET        in  : pin: u8
 *              out : level: u8
 *   SET_MASK   in  : mask: u64 LE, values: u64 LE
 *              out : (none)
 *   GET_MASK   in  : mask: u64 LE
 *              out : levels: u64 LE
 *
 * WATCH/UNWATCH are reserved for the async-event channel (M12 in
 * the project roadmap) and not implemented here — the gpio_chip
 * does not advertise irq support.
 *
 * Kernel mapping: each user-exposed firmware pin is presented as a
 * gpio_chip line whose offset matches the LIST array index. So a
 * board exposing pins {GP30, GP31, GP37, GP38, GP39, GP40, GP41,
 * GP42, GP43, GP44, GP45} appears in /dev/gpiochipN with 11 lines
 * numbered 0..10. Userspace tooling (libgpiod, gpioget/set) talks
 * to those line numbers; the driver translates back to the
 * firmware's absolute pin numbers per call.
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/driver.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "auxbus.h"
#include "rpbp.h"
#include "rpbridge.h"
#include "transport.h"

/* Maximum line count we accept from a single LIST. The firmware
 * caps it at GPIO_HW_NUM_USER_PINS = 11 today; over-cap to 64 here
 * so a future revision can grow the bank without us forking. */
#define RPBRIDGE_GPIO_MAX_LINES   64u

struct rpbridge_gpio {
	struct rpbridge_aux_dev *aux;
	struct gpio_chip         chip;
	struct mutex             lock;       /* guards pinmap + fw transfers */
	u8                       pinmap[RPBRIDGE_GPIO_MAX_LINES];
	unsigned int             ngpio;
};

/* ───── Wire helpers ─────────────────────────────────────────────── */

static int gpio_fw_list(struct rpbridge_dev *parent,
			u8 *pinmap, unsigned int *ngpio_out)
{
	u8 reply[1 + RPBRIDGE_GPIO_MAX_LINES];
	size_t reply_len = 0;
	unsigned int n;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_GPIO, RPBP_GPIO_LIST,
				  NULL, 0,
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len < 1)
		return -EPROTO;
	n = reply[0];
	if (n > RPBRIDGE_GPIO_MAX_LINES)
		return -EOVERFLOW;
	if (reply_len < (size_t)1 + n)
		return -EPROTO;

	memcpy(pinmap, &reply[1], n);
	*ngpio_out = n;
	return 0;
}

static int gpio_fw_configure(struct rpbridge_dev *parent,
			     u8 pin, u8 dir, u8 pull)
{
	u8 args[3] = { pin, dir, pull };
	size_t reply_len = 0;
	u8 dummy;

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_GPIO, RPBP_GPIO_CONFIGURE,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int gpio_fw_set(struct rpbridge_dev *parent, u8 pin, u8 value)
{
	u8 args[2] = { pin, value };
	size_t reply_len = 0;
	u8 dummy;

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_GPIO, RPBP_GPIO_SET,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int gpio_fw_get(struct rpbridge_dev *parent, u8 pin, u8 *level_out)
{
	u8 args[1] = { pin };
	u8 reply[1];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_GPIO, RPBP_GPIO_GET,
				  args, sizeof(args),
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != 1)
		return -EPROTO;
	*level_out = reply[0] & 1;
	return 0;
}

/* ───── gpio_chip callbacks ──────────────────────────────────────── */

static struct rpbridge_gpio *to_rpgpio(struct gpio_chip *chip)
{
	return gpiochip_get_data(chip);
}

static int rpbridge_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct rpbridge_gpio *g = to_rpgpio(chip);
	u8 level = 0;
	int ret;

	if (!g || !g->aux || !g->aux->parent)
		return -ENODEV;
	if (offset >= g->ngpio)
		return -EINVAL;

	mutex_lock(&g->lock);
	ret = gpio_fw_get(g->aux->parent, g->pinmap[offset], &level);
	mutex_unlock(&g->lock);
	if (ret)
		return ret;
	return (int)level;
}

static int rpbridge_gpio_set(struct gpio_chip *chip, unsigned int offset,
			     int value)
{
	struct rpbridge_gpio *g = to_rpgpio(chip);
	int ret;

	if (!g || !g->aux || !g->aux->parent)
		return -ENODEV;
	if (offset >= g->ngpio)
		return -EINVAL;

	mutex_lock(&g->lock);
	ret = gpio_fw_set(g->aux->parent, g->pinmap[offset],
			  value ? 1 : 0);
	mutex_unlock(&g->lock);
	return ret;
}

static int rpbridge_gpio_direction_input(struct gpio_chip *chip,
					 unsigned int offset)
{
	struct rpbridge_gpio *g = to_rpgpio(chip);
	int ret;

	if (!g || !g->aux || !g->aux->parent)
		return -ENODEV;
	if (offset >= g->ngpio)
		return -EINVAL;

	mutex_lock(&g->lock);
	ret = gpio_fw_configure(g->aux->parent, g->pinmap[offset],
				RPBP_GPIO_DIR_INPUT, RPBP_GPIO_PULL_NONE);
	mutex_unlock(&g->lock);
	return ret;
}

static int rpbridge_gpio_direction_output(struct gpio_chip *chip,
					  unsigned int offset, int value)
{
	struct rpbridge_gpio *g = to_rpgpio(chip);
	int ret;

	if (!g || !g->aux || !g->aux->parent)
		return -ENODEV;
	if (offset >= g->ngpio)
		return -EINVAL;

	mutex_lock(&g->lock);
	ret = gpio_fw_configure(g->aux->parent, g->pinmap[offset],
				RPBP_GPIO_DIR_OUTPUT, RPBP_GPIO_PULL_NONE);
	if (!ret)
		ret = gpio_fw_set(g->aux->parent, g->pinmap[offset],
				  value ? 1 : 0);
	mutex_unlock(&g->lock);
	return ret;
}

/* ───── auxiliary_driver glue ────────────────────────────────────── */

static int rpbridge_gpio_probe(struct auxiliary_device *auxdev,
			       const struct auxiliary_device_id *id)
{
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);
	struct rpbridge_gpio *g;
	int ret;

	(void)id;

	g = devm_kzalloc(&auxdev->dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;

	g->aux = aux;
	mutex_init(&g->lock);

	ret = gpio_fw_list(aux->parent, g->pinmap, &g->ngpio);
	if (ret) {
		dev_err(&auxdev->dev,
			"rpbridge.gpio: GPIO_LIST failed: %d\n", ret);
		goto err_destroy_lock;
	}
	if (g->ngpio == 0) {
		dev_warn(&auxdev->dev,
			 "rpbridge.gpio: firmware exposes zero GPIO lines\n");
		ret = -ENODEV;
		goto err_destroy_lock;
	}

	g->chip.label             = "rpbridge-gpio";
	g->chip.parent            = &auxdev->dev;
	g->chip.owner             = THIS_MODULE;
	g->chip.base              = -1;          /* dynamic */
	g->chip.ngpio             = g->ngpio;
	g->chip.can_sleep         = true;        /* USB transfers sleep */
	g->chip.get               = rpbridge_gpio_get;
	g->chip.set              = rpbridge_gpio_set;
	g->chip.direction_input   = rpbridge_gpio_direction_input;
	g->chip.direction_output  = rpbridge_gpio_direction_output;

	ret = devm_gpiochip_add_data(&auxdev->dev, &g->chip, g);
	if (ret) {
		dev_err(&auxdev->dev,
			"rpbridge.gpio: gpiochip_add failed: %d\n", ret);
		goto err_destroy_lock;
	}

	auxiliary_set_drvdata(auxdev, g);
	dev_info(&auxdev->dev,
		 "rpbridge.gpio: registered gpiochip with %u line(s)\n",
		 g->ngpio);
	return 0;

err_destroy_lock:
	mutex_destroy(&g->lock);
	return ret;
}

static void rpbridge_gpio_remove(struct auxiliary_device *auxdev)
{
	struct rpbridge_gpio *g = auxiliary_get_drvdata(auxdev);

	if (!g)
		return;
	/* devm_gpiochip_add_data + devm_kzalloc clean up automatically.
	 * The mutex is the only manually-created resource. */
	mutex_destroy(&g->lock);
}

static const struct auxiliary_device_id rpbridge_gpio_id_table[] = {
	{ .name = "rpbridge.gpio" },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(auxiliary, rpbridge_gpio_id_table);

static struct auxiliary_driver rpbridge_gpio_aux_driver = {
	.name        = "gpio",
	.id_table    = rpbridge_gpio_id_table,
	.probe       = rpbridge_gpio_probe,
	.remove      = rpbridge_gpio_remove,
};

int rpbridge_gpio_register(void)
{
	return auxiliary_driver_register(&rpbridge_gpio_aux_driver);
}

void rpbridge_gpio_unregister(void)
{
	auxiliary_driver_unregister(&rpbridge_gpio_aux_driver);
}
