// SPDX-License-Identifier: GPL-2.0-only
/*
 * CMC6230R LCD drm_panel driver
 *
 * Robert Yang <decatf@gmail.com>
 * Based on drivers/video/tegra/cmc623.c
 *
 * COPYRIGHT(C) Samsung Electronics Co., Ltd. 2006-2010 All Right Reserved.
 *
 */

#include <linux/backlight.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>

#include <drm/drm_crtc.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include "panel-samsung-cmc6230r.h"


#define DIM_BRIGHTNESS			15
#define LOW_BRIGHTNESS			50
#define MID_BRIGHTNESS			150
#define MAX_BRIGHTNESS			255

#define DARK_INTENSITY			0
#define DIM_INTENSITY			50
#define LOW_INTENSITY			90
#define MID_INTENSITY			784
#define MAX_INTENSITY			1600

#define PLUT_VALUE(idx) ((plut[idx] * value / 100) & 0xFF)

enum cmc623_gpios {
	BL_RESET,
	IMA_BYPASS,
	IMA_N_RST,
	IMA_PWREN,
	IMA_SLEEP,
	LVDS_N_SHDN,
	MLCD_ON,
	MLCD_ON1,
	NUM_GPIOS,
};

struct cmc623_data {
	struct i2c_client *client;
	struct gpio_desc *gpios[NUM_GPIOS];
	struct clk *clk_parent;
	struct clk *clk;
	struct mutex tuning_mutex;
	bool suspended;
	bool initialized;

	const struct drm_display_mode *mode;
	struct drm_panel panel;

	struct backlight_device *backlight;
	unsigned int last_state;

	/* model specific properties */
	const struct cmc623_register_set *init_regs;
	int ninit_regs;
	const struct cmc623_register_set *tune_regs;
	int ntune_regs;
	void (*resume_gpios)(struct i2c_client *client);
};


static int cmc623_brightness_to_intensity(struct i2c_client *client,
	int brightness);


static int cmc623_panel_type = CMC623_TYPE_LSI;

#ifdef MODULE
module_param(cmc623_panel_type, int, 0644);
#else
static int __init cmc623_arg(char *p)
{
	pr_info("%s: panel type=cmc623f\n", __func__);
	cmc623_panel_type = CMC623_TYPE_FUJITSU;
	return 0;
}
early_param("CMC623F", cmc623_arg);
#endif

static int cmc623_write_reg(struct i2c_client *client,
	unsigned char addr, unsigned int data)
{
	struct i2c_msg msg[1];
	unsigned char buf[3];
	int err;

	buf[0] = addr;
	buf[1] = (data >> 8) & 0xFF;
	buf[2] = data & 0xFF;

	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = ARRAY_SIZE(buf);
	msg->buf = buf;

	err = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (err < 0) {
		dev_err(&client->dev, "i2c_transfer failed. err = %d, addr = %x, "
			"data = %x\n", err, addr, data);
		return err;
	}

	return 0;
}

static int cmc623_write_regs(struct i2c_client *client,
	const struct cmc623_register_set *regs, int nregs)
{
	int err, i;

	for (i = 0; i < nregs; i++) {
		err = cmc623_write_reg(client, regs[i].addr, regs[i].data);
		if (err)
			break;

		if (regs[i].addr == CMC623_REG_SWRESET && regs[i].data == 0XFFFF)
			usleep_range(2000, 2100);
	}

	return err;
}

static void cmc623_pwm_cabc(struct i2c_client *client, int value)
{
	struct cmc623_data *data = i2c_get_clientdata(client);
	const unsigned char *plut = cmc623_default_plut;
	unsigned int min_duty = PLUT_VALUE(7);

	dev_dbg(&client->dev, "pwm = %d\n", value);

	mutex_lock(&data->tuning_mutex);
	cmc623_write_reg(client, 0x00, 0x0000);

	if (min_duty < 4) {
		cmc623_write_reg(client, 0xB4, 0xc000 | max(1, PLUT_VALUE(3)));
	} else {
		cmc623_write_reg(client, 0x76, (PLUT_VALUE(0) << 8) | PLUT_VALUE(1));
		cmc623_write_reg(client, 0x77, (PLUT_VALUE(2) << 8) | PLUT_VALUE(3));
		cmc623_write_reg(client, 0x78, (PLUT_VALUE(4) << 8) | PLUT_VALUE(5));
		cmc623_write_reg(client, 0x79, (PLUT_VALUE(6) << 8) | PLUT_VALUE(7));
		cmc623_write_reg(client, 0x7a, PLUT_VALUE(8) << 8);
		cmc623_write_reg(client, 0xB4, 0x5000 | (value << 4));
	}

	cmc623_write_reg(client, 0x28, 0x0000);
	mutex_unlock(&data->tuning_mutex);
}

static void cmc623_set_backlight(struct i2c_client *client, int intensity)
{
	int pwm;

	dev_dbg(&client->dev, "intensity = %d\n", intensity);

	pwm = max(0, min(MAX_INTENSITY, intensity));
	/* scale to a range of 1 - 100 */
	pwm = max(1, (pwm * 100) / MAX_INTENSITY);
	cmc623_pwm_cabc(client, pwm);
}

static void cmc623_suspend(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	if (!data->initialized || data->suspended)
		return;

	gpiod_set_value(data->gpios[BL_RESET], 0);
	msleep(100);

	gpiod_set_value(data->gpios[IMA_SLEEP], 0);
	gpiod_set_value(data->gpios[IMA_BYPASS], 0);

	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[IMA_PWREN] , 0);
	gpiod_set_value(data->gpios[LVDS_N_SHDN], 0);

	gpiod_set_value(data->gpios[MLCD_ON1], 0);
	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[MLCD_ON], 0);

	msleep(200);
	data->suspended = true;
}

static void cmc623_resume_gpios_fujitsu(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	usleep_range(1000, 2000);
	gpiod_set_value(data->gpios[IMA_BYPASS], 1);
	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[IMA_SLEEP], 1);
	usleep_range(5000, 6000);

	gpiod_set_value(data->gpios[IMA_PWREN] , 1);
	usleep_range(5000, 6000);
}

static void cmc623_resume_gpios_lsi(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	usleep_range(1000, 2000);
	gpiod_set_value(data->gpios[IMA_PWREN] , 1);
	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[IMA_BYPASS], 1);
	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[IMA_SLEEP], 1);
	usleep_range(1000, 2000);
}

static void cmc623_resume_gpios(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	gpiod_set_value(data->gpios[IMA_N_RST], 1);
	gpiod_set_value(data->gpios[IMA_PWREN] , 0);
	gpiod_set_value(data->gpios[IMA_BYPASS], 0);
	gpiod_set_value(data->gpios[IMA_SLEEP], 0);
	gpiod_set_value(data->gpios[LVDS_N_SHDN], 0);
	gpiod_set_value(data->gpios[MLCD_ON], 0);
	gpiod_set_value(data->gpios[MLCD_ON1], 0);
	gpiod_set_value(data->gpios[BL_RESET], 0);
	msleep(200);

	gpiod_set_value(data->gpios[MLCD_ON], 1);
	usleep_range(30, 100);

	gpiod_set_value(data->gpios[MLCD_ON1], 1);

	if (data->resume_gpios)
		data->resume_gpios(client);

	gpiod_set_value(data->gpios[IMA_N_RST], 0);
	usleep_range(5000, 6000);

	gpiod_set_value(data->gpios[IMA_N_RST], 1);
	usleep_range(5000, 6000);
}

static void cmc623_resume(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	if (!data->initialized || !data->suspended)
		return;

	cmc623_resume_gpios(client);

	mutex_lock(&data->tuning_mutex);
	cmc623_write_regs(client, data->init_regs, data->ninit_regs);
	cmc623_write_regs(client, data->tune_regs, data->ntune_regs);
	mutex_unlock(&data->tuning_mutex);

	gpiod_set_value(data->gpios[LVDS_N_SHDN], 1);
	gpiod_set_value(data->gpios[BL_RESET], 1);

	data->suspended = false;
}

static void cmc623_shutdown(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	if (!data->initialized)
		return;

	gpiod_set_value(data->gpios[BL_RESET], 0);
	msleep(200);

	gpiod_set_value(data->gpios[IMA_SLEEP], 0);
	gpiod_set_value(data->gpios[IMA_BYPASS], 0);
	msleep(1);

	gpiod_set_value(data->gpios[IMA_PWREN] , 0);
	gpiod_set_value(data->gpios[LVDS_N_SHDN], 0);
	gpiod_set_value(data->gpios[MLCD_ON1], 0);
	msleep(1);

	gpiod_set_value(data->gpios[MLCD_ON], 0);
	msleep(400);
}

static int cmc623_brightness_to_intensity(struct i2c_client *client,
	int brightness)
{
	int intensity;

	if (brightness >= MID_BRIGHTNESS)
		intensity =  MID_INTENSITY +
			((brightness - MID_BRIGHTNESS) * (MAX_INTENSITY - MID_INTENSITY) /
				(MAX_BRIGHTNESS - MID_BRIGHTNESS));
	else if (brightness >= LOW_BRIGHTNESS)
		intensity = LOW_INTENSITY +
			((brightness - LOW_BRIGHTNESS) * (MID_INTENSITY - LOW_INTENSITY) /
				(MID_BRIGHTNESS - LOW_BRIGHTNESS));
	else if (brightness >= DIM_BRIGHTNESS)
		intensity = DIM_INTENSITY +
			((brightness - DIM_BRIGHTNESS) * (LOW_INTENSITY - DIM_INTENSITY) /
				(LOW_BRIGHTNESS - DIM_BRIGHTNESS));
	else if (brightness > 0)
		intensity = DARK_INTENSITY;
	else
		intensity = brightness;

	return intensity;
}

static int cmc623_update_status(struct backlight_device *backlight)
{
	struct cmc623_data *data = dev_get_drvdata(&backlight->dev);
	struct i2c_client *client = data->client;
	int brightness = backlight->props.brightness;
	int intensity;

	dev_dbg(&backlight->dev, "brightness = %d\n", brightness);

	if (!data->initialized)
		return -EBUSY;

	if (backlight->props.state & BL_CORE_FBBLANK) {
		cmc623_suspend(client);
	} else {
		if (data->last_state & BL_CORE_FBBLANK)
			cmc623_resume(client);

		intensity = cmc623_brightness_to_intensity(client, brightness);
		cmc623_set_backlight(client, intensity);
	}

	data->last_state = backlight->props.state;

	return 0;
}

static int cmc623_get_brightness(struct backlight_device *backlight)
{
	struct cmc623_data *data = dev_get_drvdata(&backlight->dev);
	struct i2c_client *client = data->client;

	return cmc623_brightness_to_intensity(client,
		backlight->props.brightness);
}

static struct backlight_ops cmc623_backlight_ops = {
	.get_brightness = cmc623_get_brightness,
	.update_status  = cmc623_update_status,
};


static const struct drm_display_mode ltn101al03_lsi_mode = {
	.clock = 68750,
	.hdisplay = 1280,
	.hsync_start = 1280 + 16,
	.hsync_end = 1280 + 16 + 48,
	.htotal = 1280 + 16 + 48 +64,
	.vdisplay = 800,
	.vsync_start = 800 + 2,
	.vsync_end = 800 + 2 + 3,
	.vtotal = 800 + 2 + 3 + 11,
};

static const struct drm_display_mode ltn101al03_fujitsu_mode = {
	.clock = 76000,
	.hdisplay = 1280,
	.hsync_start = 1280 + 16,
	.hsync_end = 1280 + 16 + 48,
	.htotal = 1280 + 16 + 48 + 64,
	.vdisplay = 800,
	.vsync_start = 800 + 6,
	.vsync_end = 800 + 6 + 18,
	.vtotal = 800 + 6 + 18 + 76,
};

static inline struct cmc623_data *panel_to_cmc623(struct drm_panel *panel)
{
	return container_of(panel, struct cmc623_data, panel);
}

static int cmc623_drm_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct cmc623_data *data = panel_to_cmc623(panel);
	const struct drm_display_mode *panel_mode = data->mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, panel_mode);
	if (!mode) {
		DRM_ERROR("failed to add mode %ux%u\n",
			panel_mode->hdisplay, panel_mode->vdisplay);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 217;
	connector->display_info.height_mm = 135;
	connector->display_info.bpc = 8;

	return 1;
}

static int cmc623_drm_unprepare(struct drm_panel *panel)
{
	struct cmc623_data *data = panel_to_cmc623(panel);
	return backlight_disable(data->backlight);
}

static int cmc623_drm_prepare(struct drm_panel *panel)
{
	struct cmc623_data *data = panel_to_cmc623(panel);
	return backlight_enable(data->backlight);
}

static const struct drm_panel_funcs cmc6230r_panel_funcs = {
	.unprepare = cmc623_drm_unprepare,
	.prepare = cmc623_drm_prepare,
	.get_modes = cmc623_drm_get_modes,
};


struct cmc623_gpio_init {
	enum cmc623_gpios id;
	const char *name;
	enum gpiod_flags flags;
} static const cmc623_gpio_init_table[] = {
	{ .id = BL_RESET, .name = "bl-reset", .flags = GPIOD_OUT_HIGH },
	{ .id = IMA_BYPASS, .name = "ima-bypass", .flags = GPIOD_OUT_HIGH },
	{ .id = IMA_N_RST, .name = "ima-n-rst", .flags = GPIOD_OUT_HIGH },
	{ .id = IMA_PWREN, .name = "ima-pwren", .flags = GPIOD_OUT_HIGH },
	{ .id = IMA_SLEEP, .name = "ima-sleep", .flags = GPIOD_OUT_HIGH },
	{ .id = LVDS_N_SHDN, .name = "lvds-n-shdn", .flags = GPIOD_OUT_HIGH },
	{ .id = MLCD_ON, .name = "mlcd-on", .flags = GPIOD_OUT_HIGH },
	{ .id = MLCD_ON1, .name = "mlcd-on1", .flags = GPIOD_OUT_HIGH },
};

static int cmc623_init_gpios(struct i2c_client *client,
	struct cmc623_data *data)
{
	struct gpio_desc *desc;
	int i, err = 0;

	for (i = 0; i < ARRAY_SIZE(cmc623_gpio_init_table); i++) {
		const struct cmc623_gpio_init *item = &cmc623_gpio_init_table[i];

		desc = devm_gpiod_get(&client->dev, item->name, item->flags);
		if (IS_ERR(desc)) {
			err = PTR_ERR(desc);
			dev_err(&client->dev, "could not get %s gpio. err = %d\n",
				item->name, err);
			return err;
		}

		data->gpios[item->id] = desc;
	}

	return err;
}

static int cmc623_initialize_clks(struct i2c_client *client,
	struct cmc623_data *data, unsigned long rate)
{
	int err;

	if (!data->clk)
		return 0;

	if (data->clk_parent) {
		err = clk_set_parent(data->clk, data->clk_parent);
		if (err) {
			dev_err(&client->dev, "Failed to set clock parent\n");
			return err;
		}
	}

	err = clk_set_rate(data->clk_parent, rate);
	if (err) {
		dev_err(&client->dev, "Failed to set clock rate.\n");
		return err;
	}

	dev_dbg(&client->dev, "parent clk rate = %lu\n",
		clk_get_rate(data->clk_parent));

	return err;
}

static int cmc623_i2c_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct cmc623_data *data;
	struct backlight_properties props;
	int err, intensity;

	data = devm_kzalloc(&client->dev, sizeof(struct cmc623_data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		goto error;
	}

	err = cmc623_init_gpios(client, data);
	if (err)
		goto error;

	data->clk = devm_clk_get(&client->dev, NULL);
	if (IS_ERR(data->clk)) {
		dev_err(&client->dev, "Failed to get clock\n");
		err = PTR_ERR(data->clk);
		goto error;
	}

	data->clk_parent = devm_clk_get(&client->dev, "parent");
	if (IS_ERR(data->clk_parent)) {
		dev_err(&client->dev, "Failed to get parent clock\n");
		err = PTR_ERR(data->clk_parent);
		goto error;
	}

	if (cmc623_panel_type == CMC623_TYPE_FUJITSU) {
		data->resume_gpios = cmc623_resume_gpios_fujitsu;
		data->init_regs = cmc623_regs_fujitsu;
		data->ninit_regs = ARRAY_SIZE(cmc623_regs_fujitsu);
		data->mode = &ltn101al03_fujitsu_mode;
	} else if (cmc623_panel_type == CMC623_TYPE_LSI) {
		data->resume_gpios = cmc623_resume_gpios_lsi;
		data->init_regs = cmc623_regs_lsi;
		data->ninit_regs = ARRAY_SIZE(cmc623_regs_lsi);
		data->mode = &ltn101al03_lsi_mode;
	} else {
		WARN(1, "Unknown panel type.");
		err = -EINVAL;
		goto error;
	}

	data->tune_regs = standard_ui_cabcon;
	data->ntune_regs = ARRAY_SIZE(standard_ui_cabcon);
	data->client = client;
	data->suspended = false;
	mutex_init(&data->tuning_mutex);
	i2c_set_clientdata(client, data);

	/* Register the drm panel */
	drm_panel_init(&data->panel, &client->dev, &cmc6230r_panel_funcs,
		       DRM_MODE_CONNECTOR_LVDS);
	drm_panel_add(&data->panel);

	/* Register the backlight */
	props.type = BACKLIGHT_RAW;

	data->backlight = backlight_device_register("pwm-backlight",
		&client->dev, data, &cmc623_backlight_ops, &props);
	if (IS_ERR(data->backlight)) {
		err = PTR_ERR(data->backlight);
		goto after_panel_error;
	}

	data->backlight->props.max_brightness = MAX_BRIGHTNESS;
	data->backlight->props.brightness = MAX_BRIGHTNESS - 128;

	data->initialized = true;

	/*
	 * The display cannot handle clock rate change while the panel
	 * is on. The bootloader will bring up the panel. Then during kernel
	 * init the clock rates can/will change resulting in mangled display.
	 * Re-initialize the panel and clock rate to ensure stable display.
	 */
	cmc623_suspend(client);
	cmc623_initialize_clks(client, data, data->mode->clock * 1000);
	cmc623_resume(client);
	intensity = cmc623_brightness_to_intensity(client,
		data->backlight->props.brightness);
	cmc623_set_backlight(client, intensity);

	return 0;

after_panel_error:
	drm_panel_remove(&data->panel);
error:
	return err;
}

static int cmc623_i2c_remove(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	data->backlight->props.brightness = 0;
	data->backlight->props.power = 0;
	cmc623_update_status(data->backlight);

	backlight_device_unregister(data->backlight);
	drm_panel_remove(&data->panel);

	mutex_destroy(&data->tuning_mutex);

	return 0;
}

static const struct i2c_device_id cmc623_id[] = {
	{ "cmc6230r", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, cmc623_id);

static const struct of_device_id cmc623_dt_match[] = {
	{ .compatible = "samsung,cmc6230r" },
	{ },
};
MODULE_DEVICE_TABLE(of, cmc623_dt_match);

struct i2c_driver cmc623_i2c_driver = {
	.driver	= {
		.name	= "cmc6230r",
		.of_match_table = cmc623_dt_match,
	},
	.probe		= cmc623_i2c_probe,
	.remove		= cmc623_i2c_remove,
	.id_table	= cmc623_id,
	.shutdown = cmc623_shutdown,
};
module_i2c_driver(cmc623_i2c_driver);

MODULE_AUTHOR("Robert Yang <decatf@gmail.com>");
MODULE_DESCRIPTION("cmc6230r LCD driver");
MODULE_LICENSE("GPL v2");
