// SPDX-License-Identifier: GPL-2.0
/*
 * C-side shims for the FFI surface defined in ffi/bindings.rs.
 *
 * Every function here does one of three things:
 *
 *   1. Expose an inline kernel helper (usb_sndbulkpipe, usb_rcvbulkpipe)
 *      as a real symbol so Rust can call it.
 *
 *   2. Read or write a struct field whose C layout is kernel-version
 *      dependent, so Rust does not need a #[repr(C)] mirror.
 *
 *   3. Allocate a typed kernel object, plumb Rust callbacks through
 *      trampolines into its vtable, and return a stable pointer.
 *
 * Keeping this C code small and mechanical means layout churn across
 * 6.13 → 6.15+ lands here, not in every Rust caller.
 */

#include <linux/usb.h>
#include <linux/auxiliary_bus.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/module.h>

#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/spi/spi.h>
#include <linux/pwm.h>
#include <linux/iio/iio.h>
#include <linux/iio/triggered_buffer.h>

/* ───── Layout sanity checks ─────────────────────────────────────────
 *
 * These static_asserts guarantee the constants we baked into the Rust
 * bindings match the kernel we're compiling against. A mismatch is a
 * build failure, not a runtime crash.
 */
_Static_assert(sizeof(wait_queue_head_t) <= 48,
	       "wait_queue_head_t size ceiling; bump RPBRIDGE_WQ_HEAD_SIZE");

/* ───── USB ──────────────────────────────────────────────────────── */

unsigned int rpbridge_usb_sndbulkpipe(struct usb_device *dev,
				      unsigned int endpoint)
{
	return usb_sndbulkpipe(dev, endpoint);
}

unsigned int rpbridge_usb_rcvbulkpipe(struct usb_device *dev,
				      unsigned int endpoint)
{
	return usb_rcvbulkpipe(dev, endpoint);
}

int rpbridge_intf_num_endpoints(struct usb_interface *intf)
{
	if (!intf || !intf->cur_altsetting)
		return 0;
	return intf->cur_altsetting->desc.bNumEndpoints;
}

u8 rpbridge_intf_endpoint_address(struct usb_interface *intf, int i)
{
	if (!intf || !intf->cur_altsetting) return 0;
	if (i < 0 || i >= intf->cur_altsetting->desc.bNumEndpoints) return 0;
	return intf->cur_altsetting->endpoint[i].desc.bEndpointAddress;
}

u8 rpbridge_intf_endpoint_attributes(struct usb_interface *intf, int i)
{
	if (!intf || !intf->cur_altsetting) return 0;
	if (i < 0 || i >= intf->cur_altsetting->desc.bNumEndpoints) return 0;
	return intf->cur_altsetting->endpoint[i].desc.bmAttributes;
}

u16 rpbridge_intf_endpoint_max_packet(struct usb_interface *intf, int i)
{
	if (!intf || !intf->cur_altsetting) return 0;
	if (i < 0 || i >= intf->cur_altsetting->desc.bNumEndpoints) return 0;
	return le16_to_cpu(intf->cur_altsetting->endpoint[i].desc.wMaxPacketSize);
}

/*
 * Parent `struct device *` to register subsystem objects (gpio_chip,
 * i2c_adapter, spi_controller, pwm_chip, iio_dev) under. We hand out
 * the USB interface's own device — subsystems hang below it in
 * /sys/bus/usb/devices/<bus>-<port>/<intf>/ which is exactly what
 * libgpiod / i2c-tools / iio userland expects.
 */
struct device *rpbridge_intf_parent_dev(struct usb_interface *intf)
{
	return &intf->dev;
}

/* ───── wait_queue ────────────────────────────────────────────────── */

void rpbridge_waitqueue_init(void *wq)
{
	init_waitqueue_head((wait_queue_head_t *)wq);
}

long rpbridge_wait_event_timeout_ms(void *wq, unsigned long timeout_ms)
{
	return wait_event_interruptible_timeout(
		*(wait_queue_head_t *)wq,
		true, /* no condition — caller checks under its own lock */
		msecs_to_jiffies(timeout_ms));
}

void rpbridge_wake_up_all(void *wq)
{
	wake_up_all((wait_queue_head_t *)wq);
}

/* ───── GPIO shim ─────────────────────────────────────────────────── *
 *
 * The gpio_chip ops are forwarded through Rust trampolines exposed by
 * core_driver.rs:
 *
 *   extern "Rust" { fn rpbridge_gpio_op_get(cookie: *mut c_void, line: u32) -> c_int; ... }
 *
 * We implement the C-side gpio_chip vtable as tiny wrappers that grab
 * the cookie from chip->data and call the Rust symbol.
 */

struct rpbridge_gpio_priv {
	struct gpio_chip chip;
	void *cookie;
};

/* Forward Rust symbols — defined via #[no_mangle] in gpio.rs. */
extern int  rpbridge_gpio_cb_get(void *cookie, unsigned int line);
extern void rpbridge_gpio_cb_set(void *cookie, unsigned int line, int value);
extern int  rpbridge_gpio_cb_direction_input(void *cookie, unsigned int line);
extern int  rpbridge_gpio_cb_direction_output(void *cookie, unsigned int line, int value);
extern int  rpbridge_gpio_cb_request(void *cookie, unsigned int line);
extern void rpbridge_gpio_cb_free(void *cookie, unsigned int line);

static int op_get(struct gpio_chip *c, unsigned int line)
{
	struct rpbridge_gpio_priv *p = gpiochip_get_data(c);
	return rpbridge_gpio_cb_get(p->cookie, line);
}
static void op_set(struct gpio_chip *c, unsigned int line, int value)
{
	struct rpbridge_gpio_priv *p = gpiochip_get_data(c);
	rpbridge_gpio_cb_set(p->cookie, line, value);
}
static int op_direction_input(struct gpio_chip *c, unsigned int line)
{
	struct rpbridge_gpio_priv *p = gpiochip_get_data(c);
	return rpbridge_gpio_cb_direction_input(p->cookie, line);
}
static int op_direction_output(struct gpio_chip *c, unsigned int line, int value)
{
	struct rpbridge_gpio_priv *p = gpiochip_get_data(c);
	return rpbridge_gpio_cb_direction_output(p->cookie, line, value);
}
static int op_request(struct gpio_chip *c, unsigned int line)
{
	struct rpbridge_gpio_priv *p = gpiochip_get_data(c);
	return rpbridge_gpio_cb_request(p->cookie, line);
}
static void op_free(struct gpio_chip *c, unsigned int line)
{
	struct rpbridge_gpio_priv *p = gpiochip_get_data(c);
	rpbridge_gpio_cb_free(p->cookie, line);
}

struct gpio_chip *rpbridge_gpio_chip_new(struct device *parent,
					  const char *label,
					  int base, u16 ngpio,
					  void *cookie)
{
	struct rpbridge_gpio_priv *p =
		kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p) return NULL;

	p->cookie              = cookie;
	p->chip.label          = label;
	p->chip.parent         = parent;
	p->chip.owner          = THIS_MODULE;
	p->chip.base           = base;
	p->chip.ngpio          = ngpio;
	p->chip.can_sleep      = true; /* USB transfer in our callbacks */
	p->chip.get            = op_get;
	p->chip.set            = op_set;
	p->chip.direction_input  = op_direction_input;
	p->chip.direction_output = op_direction_output;
	p->chip.request        = op_request;
	p->chip.free           = op_free;
	return &p->chip;
}

void rpbridge_gpio_chip_free(struct gpio_chip *chip)
{
	struct rpbridge_gpio_priv *p =
		container_of(chip, struct rpbridge_gpio_priv, chip);
	kfree(p);
}

int rpbridge_gpio_chip_register(struct gpio_chip *chip)
{
	struct rpbridge_gpio_priv *p =
		container_of(chip, struct rpbridge_gpio_priv, chip);
	return gpiochip_add_data(chip, p);
}

void rpbridge_gpio_chip_unregister(struct gpio_chip *chip)
{
	gpiochip_remove(chip);
}

int rpbridge_gpio_girq_add(struct gpio_chip *chip)
{
	/* Nested irq_chip configured by gpio_irq_chip in the parent
	 * struct. In this first cut we register without an irq_chip —
	 * WATCH events are delivered as uevents through gpiolib's
	 * default edge-event path. A proper nested irq_chip is future
	 * work once libgpiod users demand it.
	 */
	(void)chip;
	return 0;
}

void rpbridge_gpio_handle_edge(struct gpio_chip *chip, unsigned int line)
{
	(void)chip; (void)line;
	/* Placeholder — wake any poll(2) waiters on /dev/gpiochip%d.
	 * Real impl calls generic_handle_domain_irq() once the irq_chip
	 * is wired. */
}

/* ───── I²C shim ─────────────────────────────────────────────────── */

struct rpbridge_i2c_priv {
	struct i2c_adapter adap;
	void *cookie;
};

extern int rpbridge_i2c_cb_master_xfer(
	void *cookie, struct i2c_msg *msgs, int num);
extern u32 rpbridge_i2c_cb_functionality(void *cookie);

static int adap_master_xfer(struct i2c_adapter *a, struct i2c_msg *msgs, int num)
{
	struct rpbridge_i2c_priv *p =
		container_of(a, struct rpbridge_i2c_priv, adap);
	return rpbridge_i2c_cb_master_xfer(p->cookie, msgs, num);
}
static u32 adap_functionality(struct i2c_adapter *a)
{
	struct rpbridge_i2c_priv *p =
		container_of(a, struct rpbridge_i2c_priv, adap);
	return rpbridge_i2c_cb_functionality(p->cookie);
}

static const struct i2c_algorithm rpbridge_i2c_algo = {
	.master_xfer   = adap_master_xfer,
	.functionality = adap_functionality,
};

struct i2c_adapter *rpbridge_i2c_adapter_new(struct device *parent,
					      const char *name,
					      void *cookie)
{
	struct rpbridge_i2c_priv *p =
		kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p) return NULL;
	p->cookie = cookie;
	p->adap.owner = THIS_MODULE;
	p->adap.algo  = &rpbridge_i2c_algo;
	p->adap.dev.parent = parent;
	strscpy(p->adap.name, name, sizeof(p->adap.name));
	return &p->adap;
}

int rpbridge_i2c_adapter_register(struct i2c_adapter *adap)
{
	return i2c_add_adapter(adap);
}

void rpbridge_i2c_adapter_free(struct i2c_adapter *adap)
{
	struct rpbridge_i2c_priv *p =
		container_of(adap, struct rpbridge_i2c_priv, adap);
	i2c_del_adapter(adap);
	kfree(p);
}

u16 rpbridge_i2c_msg_addr (struct i2c_msg *m) { return m->addr;  }
u16 rpbridge_i2c_msg_flags(struct i2c_msg *m) { return m->flags; }
u16 rpbridge_i2c_msg_len  (struct i2c_msg *m) { return m->len;   }
u8 *rpbridge_i2c_msg_buf  (struct i2c_msg *m) { return m->buf;   }

/* ───── SPI shim ─────────────────────────────────────────────────── */

struct rpbridge_spi_priv {
	struct spi_controller *ctlr;
	void *cookie;
};

extern int rpbridge_spi_cb_transfer_one_message(
	void *cookie, struct spi_message *msg);

static int ctlr_transfer_one_message(struct spi_controller *c,
				     struct spi_message *msg)
{
	struct rpbridge_spi_priv *p = spi_controller_get_devdata(c);
	int ret = rpbridge_spi_cb_transfer_one_message(p->cookie, msg);
	/* The SPI core contract requires transfer_one_message to finalize
	 * the message before returning — otherwise the next request stays
	 * queued forever. Rust callers set `ret` but don't know about
	 * finalize_current_message, so we do it here. */
	msg->status = ret;
	spi_finalize_current_message(c);
	return ret;
}

struct spi_controller *rpbridge_spi_controller_new(struct device *parent,
						    u16 num_chipselect,
						    void *cookie)
{
	struct spi_controller *ctlr;
	struct rpbridge_spi_priv *p;

	/* spi_alloc_host is the post-6.17 name for spi_alloc_master. The
	 * old symbol is still re-exported via a compat shim as of 6.18
	 * but we pick the modern name to stay warning-free on upstream
	 * kernels. */
	ctlr = spi_alloc_host(parent, sizeof(struct rpbridge_spi_priv));
	if (!ctlr) return NULL;

	p = spi_controller_get_devdata(ctlr);
	p->ctlr   = ctlr;
	p->cookie = cookie;

	ctlr->num_chipselect       = num_chipselect;
	ctlr->mode_bits            = SPI_CPOL | SPI_CPHA;
	ctlr->bits_per_word_mask   = SPI_BPW_MASK(8);
	ctlr->transfer_one_message = ctlr_transfer_one_message;
	/* We drive CS via the firmware (FLAG_HOLD_CS), so controller
	 * core should not manage CS GPIOs at all. */
	ctlr->use_gpio_descriptors = false;
	return ctlr;
}

int rpbridge_spi_controller_register(struct spi_controller *ctlr)
{
	return spi_register_controller(ctlr);
}

void rpbridge_spi_controller_free(struct spi_controller *ctlr)
{
	spi_unregister_controller(ctlr);
	spi_controller_put(ctlr);
}

struct spi_transfer *rpbridge_spi_msg_first_xfer(struct spi_message *msg)
{
	return list_first_entry_or_null(&msg->transfers, struct spi_transfer, transfer_list);
}

struct spi_transfer *rpbridge_spi_msg_next_xfer(struct spi_message *msg,
						 struct spi_transfer *cur)
{
	if (list_is_last(&cur->transfer_list, &msg->transfers))
		return NULL;
	return list_next_entry(cur, transfer_list);
}

const u8 *rpbridge_spi_xfer_tx_buf(struct spi_transfer *x) { return x->tx_buf; }
u8       *rpbridge_spi_xfer_rx_buf(struct spi_transfer *x) { return x->rx_buf; }
u32       rpbridge_spi_xfer_len   (struct spi_transfer *x) { return x->len;     }
bool      rpbridge_spi_xfer_cs_change(struct spi_transfer *x) { return x->cs_change; }

u8 rpbridge_spi_dev_cs_pin(struct spi_device *d)
{
	/* spi_get_chipselect() replaces the direct d->chip_select access
	 * in 6.17+. Still valid on 6.18+; we don't maintain pre-6.17
	 * compat because kernel-baseline requires 6.18. */
	return (u8)spi_get_chipselect(d, 0);
}

void rpbridge_spi_finalize_current_message(struct spi_controller *ctlr)
{
	spi_finalize_current_message(ctlr);
}

/* ───── PWM shim ─────────────────────────────────────────────────── *
 *
 * pwm_chip layout has been private since 6.10; drivers must allocate
 * via pwmchip_alloc(parent, npwm, sizeof_priv) and attach a cookie via
 * pwmchip_priv(chip). container_of + kzalloc no longer works on 6.18.
 */

extern int rpbridge_pwm_cb_apply(void *cookie, unsigned int hwpwm,
				  const struct pwm_state *state);
extern int rpbridge_pwm_cb_get_state(void *cookie, unsigned int hwpwm,
				      struct pwm_state *state);

static int ops_apply(struct pwm_chip *c, struct pwm_device *d,
		     const struct pwm_state *state)
{
	void *cookie = *(void **)pwmchip_priv(c);
	return rpbridge_pwm_cb_apply(cookie, d->hwpwm, state);
}
static int ops_get_state(struct pwm_chip *c, struct pwm_device *d,
			 struct pwm_state *state)
{
	void *cookie = *(void **)pwmchip_priv(c);
	return rpbridge_pwm_cb_get_state(cookie, d->hwpwm, state);
}

static const struct pwm_ops rpbridge_pwm_ops = {
	.apply     = ops_apply,
	.get_state = ops_get_state,
};

struct pwm_chip *rpbridge_pwm_chip_new(struct device *parent,
					unsigned int npwm, void *cookie)
{
	struct pwm_chip *chip = pwmchip_alloc(parent, npwm, sizeof(void *));

	if (IS_ERR_OR_NULL(chip))
		return NULL;

	*(void **)pwmchip_priv(chip) = cookie;
	chip->ops = &rpbridge_pwm_ops;
	return chip;
}

int  rpbridge_pwm_chip_register(struct pwm_chip *c) { return pwmchip_add(c); }
void rpbridge_pwm_chip_free(struct pwm_chip *c)
{
	pwmchip_remove(c);
	pwmchip_put(c);
}

u64  rpbridge_pwm_state_period_ns(struct pwm_state *s) { return s->period; }
u64  rpbridge_pwm_state_duty_ns  (struct pwm_state *s) { return s->duty_cycle; }
bool rpbridge_pwm_state_enabled  (struct pwm_state *s) { return s->enabled; }
int  rpbridge_pwm_state_polarity (struct pwm_state *s) { return (int)s->polarity; }

void rpbridge_pwm_state_set_period(struct pwm_state *s, u64 v)   { s->period = v; }
void rpbridge_pwm_state_set_duty  (struct pwm_state *s, u64 v)   { s->duty_cycle = v; }
void rpbridge_pwm_state_set_enabled(struct pwm_state *s, bool v) { s->enabled = v; }
void rpbridge_pwm_state_set_polarity(struct pwm_state *s, int v) { s->polarity = v; }

unsigned int rpbridge_pwm_dev_hwpwm(struct pwm_device *d) { return d->hwpwm; }

/* ───── IIO shim ─────────────────────────────────────────────────── *
 *
 * The iio_chan_spec array below describes the fixed channel map on
 * rev-B: 4 voltage channels on GP26..GP29 + 1 on-die temperature.
 * Re-define this array alongside the firmware if the channel map
 * changes; the Rust side reads samples via channel index so the
 * indices here must match the firmware's READ / TEMP_READ contract.
 */

struct rpbridge_iio_priv {
	struct iio_dev *indio;
	void *cookie;
};

extern int rpbridge_iio_cb_read_raw(
	void *cookie, int ch, int info, int *val, int *val2);

static int indio_read_raw(struct iio_dev *indio,
			  struct iio_chan_spec const *chan,
			  int *val, int *val2, long mask)
{
	struct rpbridge_iio_priv *p = iio_priv(indio);
	return rpbridge_iio_cb_read_raw(p->cookie, chan->channel, (int)mask,
					 val, val2);
}

static const struct iio_info rpbridge_iio_info = {
	.read_raw = indio_read_raw,
};

/* Voltage channels 0..3 expose RAW + SCALE; SCALE is type-shared so
 * userspace only reads `in_voltage_scale` once. Temperature channel 4
 * exposes PROCESSED (in millidegrees C, as the IIO convention wants). */
static const struct iio_chan_spec rpbridge_iio_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.channel = 0,
		.indexed = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_VOLTAGE,
		.channel = 1,
		.indexed = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_VOLTAGE,
		.channel = 2,
		.indexed = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_VOLTAGE,
		.channel = 3,
		.indexed = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_TEMP,
		.channel = 4,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

struct iio_dev *rpbridge_iio_device_new(struct device *parent,
					 unsigned int num_channels,
					 void *cookie)
{
	struct iio_dev *indio = devm_iio_device_alloc(parent,
						       sizeof(struct rpbridge_iio_priv));
	struct rpbridge_iio_priv *p;

	if (!indio) return NULL;
	p = iio_priv(indio);
	p->indio  = indio;
	p->cookie = cookie;

	indio->name     = "rpbridge-iio";
	indio->modes    = INDIO_DIRECT_MODE;
	indio->info     = &rpbridge_iio_info;

	/* Clamp the caller-supplied num_channels to our canonical table.
	 * The Rust side passes 5 (rev-B); anything else is rejected. */
	if (num_channels != ARRAY_SIZE(rpbridge_iio_channels)) {
		return NULL;
	}
	indio->channels     = rpbridge_iio_channels;
	indio->num_channels = ARRAY_SIZE(rpbridge_iio_channels);
	return indio;
}

int  rpbridge_iio_device_register(struct iio_dev *indio)
{
	return iio_device_register(indio);
}
void rpbridge_iio_device_free(struct iio_dev *indio)
{
	iio_device_unregister(indio);
	/* devm_iio_device_alloc: no explicit free. */
}

/*
 * Triggered-buffer support is an M12 deliverable that needs a
 * dedicated kthread feeding STREAM_DATA frames into the IIO buffer.
 * The M11 driver is single-shot-only, so these functions are stubs
 * that return -ENOSYS. Callers (there are none in the current Rust
 * code path) therefore fail loudly instead of crashing on a
 * half-configured triggered buffer.
 */
int rpbridge_iio_triggered_buffer_setup(struct iio_dev *indio)
{
	(void)indio;
	return -ENOSYS;
}

void rpbridge_iio_triggered_buffer_teardown(struct iio_dev *indio)
{
	(void)indio;
}

void rpbridge_iio_push_sample_block(struct iio_dev *indio,
				     const u16 *samples,
				     unsigned int n_samples,
				     u64 ts_ns)
{
	(void)indio; (void)samples; (void)n_samples; (void)ts_ns;
	/* M12: push samples via iio_push_to_buffers_with_timestamp()
	 * once the async event kthread is wired up. */
}
