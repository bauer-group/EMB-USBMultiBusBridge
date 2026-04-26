// SPDX-License-Identifier: GPL-2.0
/*
 * PWM subsystem — auxiliary_driver that registers a pwm_chip backed
 * by the firmware's PWM subsystem (RPBP 0x04).
 *
 * Wire layer mirrors firmware/src/commands/cmd_pwm.c:
 *
 *   CONFIGURE  in  pin:u8 flags:u8 duty_q16:u16le freq_hz:u32le
 *   SET_DUTY   in  pin:u8 duty_q16:u16le
 *   START      in  pin:u8
 *   STOP       in  pin:u8
 *   GET_STATE  in  pin:u8
 *              out freq_hz:u32le duty_q16:u16le running:u8 invert:u8
 *                  slice:u8 channel:u8
 *
 * Duty is Q0.16 — duty_q16 / 65536 = fractional duty cycle.
 * The kernel pwm_state expresses duty as period_ns × duty_ns; we
 * convert at the boundary.
 *
 * The pwm_chip line offsets correspond to the firmware's user-pin
 * whitelist (boards expose 6 PWM-capable pins on rev-B today). The
 * GPIO subsystem already enumerates the user pins via GPIO_LIST;
 * for PWM we just trust the cap-map's bus_count and map line offset
 * 0..N-1 to pin GP30, GP31, GP37, GP38, GP39, GP40 in firmware
 * order. A future GET_STATE-driven enumeration could replace this
 * with a dynamic map.
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "auxbus.h"
#include "rpbp.h"
#include "rpbridge.h"
#include "transport.h"

/* Maximum PWM lines we accept. Firmware's whitelist tops out at 6
 * on rev-B; over-cap to 16 here for forward-compat. */
#define RPBRIDGE_PWM_MAX_LINES   16u

/* Boards' pin whitelist matches what cmd_pwm.c maps. We mirror it
 * here so the driver doesn't need a per-board capability fetch.
 * If the firmware's whitelist changes, this needs updating in
 * lockstep — track via a wire-level GET_STATE on probe in a future
 * commit. */
static const u8 rpbridge_pwm_pins[] = {
	30, 31, 37, 38, 39, 40,
};

struct rpbridge_pwm {
	struct rpbridge_aux_dev *aux;
	struct pwm_chip         *chip;
};

/* ───── Wire helpers ─────────────────────────────────────────────── */

static int pwm_fw_configure(struct rpbridge_dev *parent,
			    u8 pin, u8 flags, u16 duty_q16, u32 freq_hz)
{
	u8 args[8];
	size_t reply_len = 0;
	u8 dummy;

	args[0] = pin;
	args[1] = flags;
	args[2] = (u8)(duty_q16 & 0xFF);
	args[3] = (u8)((duty_q16 >> 8) & 0xFF);
	args[4] = (u8)(freq_hz & 0xFF);
	args[5] = (u8)((freq_hz >> 8) & 0xFF);
	args[6] = (u8)((freq_hz >> 16) & 0xFF);
	args[7] = (u8)((freq_hz >> 24) & 0xFF);

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_PWM, RPBP_PWM_CONFIGURE,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

/*
 * Currently unused — pwm_apply() goes through pwm_fw_configure()
 * which sets period+duty atomically. Kept around (and marked
 * maybe_unused) because a fast-path "just change the duty" helper
 * is the obvious optimisation for a future runtime-PM patch and
 * removing it now would just mean writing it again.
 */
static __maybe_unused int pwm_fw_set_duty(struct rpbridge_dev *parent,
					   u8 pin, u16 duty_q16)
{
	u8 args[3];
	size_t reply_len = 0;
	u8 dummy;

	args[0] = pin;
	args[1] = (u8)(duty_q16 & 0xFF);
	args[2] = (u8)((duty_q16 >> 8) & 0xFF);

	return rpbridge_round_trip(parent,
				   RPBP_SUBSYS_PWM, RPBP_PWM_SET_DUTY,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int pwm_fw_start_stop(struct rpbridge_dev *parent, u8 pin, bool start)
{
	u8 args[1] = { pin };
	size_t reply_len = 0;
	u8 dummy;

	return rpbridge_round_trip(parent, RPBP_SUBSYS_PWM,
				   start ? RPBP_PWM_START : RPBP_PWM_STOP,
				   args, sizeof(args),
				   &dummy, 0, &reply_len);
}

static int pwm_fw_get_state(struct rpbridge_dev *parent, u8 pin,
			    u32 *freq_hz, u16 *duty_q16,
			    u8 *running, u8 *invert)
{
	u8 args[1] = { pin };
	u8 reply[10];
	size_t reply_len = 0;
	int ret;

	ret = rpbridge_round_trip(parent,
				  RPBP_SUBSYS_PWM, RPBP_PWM_GET_STATE,
				  args, sizeof(args),
				  reply, sizeof(reply), &reply_len);
	if (ret)
		return ret;
	if (reply_len != sizeof(reply))
		return -EPROTO;

	*freq_hz = (u32)reply[0]
		 | ((u32)reply[1] << 8)
		 | ((u32)reply[2] << 16)
		 | ((u32)reply[3] << 24);
	*duty_q16 = (u16)reply[4] | ((u16)reply[5] << 8);
	*running  = reply[6];
	*invert   = reply[7];
	/* slice (reply[8]) and channel (reply[9]) are firmware-internal
	 * routing; we don't surface them in pwm_state. */
	return 0;
}

/* ───── pwm_chip ops ─────────────────────────────────────────────── */

static int rpbridge_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			      const struct pwm_state *state)
{
	struct rpbridge_pwm *p = pwmchip_get_drvdata(chip);
	u8 pin, flags = 0;
	u32 freq_hz = 0;
	u16 duty_q16 = 0;
	int ret;

	if (!p || !p->aux || !p->aux->parent)
		return -ENODEV;
	if (pwm->hwpwm >= ARRAY_SIZE(rpbridge_pwm_pins))
		return -EINVAL;

	pin = rpbridge_pwm_pins[pwm->hwpwm];

	if (state->polarity == PWM_POLARITY_INVERSED)
		flags |= RPBP_PWM_FLAG_INVERT;

	if (state->period > 0) {
		/* Convert ns to Hz with rounding. The firmware accepts
		 * 8 Hz .. 100 kHz; clamp by returning -ERANGE so spec
		 * violations from userspace surface cleanly. */
		u64 hz = NSEC_PER_SEC;

		do_div(hz, state->period);
		if (hz < 8 || hz > 100000)
			return -ERANGE;
		freq_hz = (u32)hz;
	}

	if (state->duty_cycle && state->period) {
		/* duty_q16 = (duty_ns / period_ns) * 65536, rounding. */
		u64 q = (u64)state->duty_cycle << 16;

		do_div(q, state->period);
		if (q > 0xFFFF)
			q = 0xFFFF;
		duty_q16 = (u16)q;
	}

	ret = pwm_fw_configure(p->aux->parent, pin, flags, duty_q16, freq_hz);
	if (ret)
		return ret;

	ret = pwm_fw_start_stop(p->aux->parent, pin, state->enabled);
	if (ret)
		return ret;
	return 0;
}

static int rpbridge_pwm_get_state(struct pwm_chip *chip,
				  struct pwm_device *pwm,
				  struct pwm_state *state)
{
	struct rpbridge_pwm *p = pwmchip_get_drvdata(chip);
	u8 pin, running = 0, invert = 0;
	u32 freq_hz = 0;
	u16 duty_q16 = 0;
	int ret;

	if (!p || !p->aux || !p->aux->parent)
		return -ENODEV;
	if (pwm->hwpwm >= ARRAY_SIZE(rpbridge_pwm_pins))
		return -EINVAL;

	pin = rpbridge_pwm_pins[pwm->hwpwm];
	ret = pwm_fw_get_state(p->aux->parent, pin,
			       &freq_hz, &duty_q16, &running, &invert);
	if (ret)
		return ret;

	state->period = freq_hz ? (u64)NSEC_PER_SEC / freq_hz : 0;
	if (state->period && duty_q16) {
		u64 d = (u64)state->period * duty_q16;

		state->duty_cycle = (u32)(d >> 16);
	} else {
		state->duty_cycle = 0;
	}
	state->enabled  = !!running;
	state->polarity = invert ? PWM_POLARITY_INVERSED
				 : PWM_POLARITY_NORMAL;
	return 0;
}

static const struct pwm_ops rpbridge_pwm_ops = {
	.apply      = rpbridge_pwm_apply,
	.get_state  = rpbridge_pwm_get_state,
};

/* ───── auxiliary_driver glue ────────────────────────────────────── */

static int rpbridge_pwm_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct rpbridge_aux_dev *aux = container_of(auxdev,
						    struct rpbridge_aux_dev,
						    auxdev);
	struct rpbridge_pwm *p;
	struct pwm_chip *chip;
	int ret;

	(void)id;

	chip = devm_pwmchip_alloc(&auxdev->dev,
				  ARRAY_SIZE(rpbridge_pwm_pins),
				  sizeof(*p));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	p = pwmchip_get_drvdata(chip);
	p->aux  = aux;
	p->chip = chip;

	chip->ops = &rpbridge_pwm_ops;

	ret = devm_pwmchip_add(&auxdev->dev, chip);
	if (ret)
		return ret;

	auxiliary_set_drvdata(auxdev, p);
	dev_info(&auxdev->dev,
		 "rpbridge.pwm: registered pwmchip with %u line(s)\n",
		 (unsigned int)ARRAY_SIZE(rpbridge_pwm_pins));
	return 0;
}

static void rpbridge_pwm_remove(struct auxiliary_device *auxdev)
{
	(void)auxdev; /* devm-everything */
}

static const struct auxiliary_device_id rpbridge_pwm_id_table[] = {
	{ .name = "rpbridge.pwm" },
	{ /* terminator */ }
};
MODULE_DEVICE_TABLE(auxiliary, rpbridge_pwm_id_table);

static struct auxiliary_driver rpbridge_pwm_aux_driver = {
	.name        = "pwm",
	.id_table    = rpbridge_pwm_id_table,
	.probe       = rpbridge_pwm_probe,
	.remove      = rpbridge_pwm_remove,
};

int rpbridge_pwm_register(void)
{
	return auxiliary_driver_register(&rpbridge_pwm_aux_driver);
}

void rpbridge_pwm_unregister(void)
{
	auxiliary_driver_unregister(&rpbridge_pwm_aux_driver);
}
