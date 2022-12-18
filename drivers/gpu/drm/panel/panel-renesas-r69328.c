// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas R69328 panel driver
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define R69328_MACP		0xB0 /* Manufacturer Command Access Protect */

#define R69328_GAMMA_SET_A	0xC8 /* Gamma Setting A */
#define R69328_GAMMA_SET_B	0xC9 /* Gamma Setting B */
#define R69328_GAMMA_SET_C	0xCA /* Gamma Setting C */

#define R69328_POWER_SET	0xD1

struct renesas_r69328 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;

	struct regulator *vdd_supply;
	struct regulator *vddio_supply;
	struct gpio_desc *reset_gpio;

	bool prepared;
};

static const u8 address_mode[] = {
	MIPI_DCS_SET_ADDRESS_MODE
};

static inline struct renesas_r69328 *to_renesas_r69328(struct drm_panel *panel)
{
	return container_of(panel, struct renesas_r69328, panel);
}

#define dsi_generic_write_seq(dsi, cmd, seq...) do {			\
		static const u8 b[] = { cmd, seq };			\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, b, ARRAY_SIZE(b));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static void renesas_r69328_reset(struct renesas_r69328 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(2000, 3000);
}

static int renesas_r69328_prepare(struct drm_panel *panel)
{
	struct renesas_r69328 *ctx = to_renesas_r69328(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	ret = regulator_enable(ctx->vdd_supply);
	if (ret < 0) {
		dev_err(dev, "failed to enable vdd power supply\n");
		return ret;
	}

	usleep_range(10000, 11000);

	ret = regulator_enable(ctx->vddio_supply);
	if (ret < 0) {
		dev_err(dev, "failed to enable vddio power supply\n");
		return ret;
	}

	usleep_range(10000, 11000);

	renesas_r69328_reset(ctx);

	ctx->prepared = true;
	return 0;
}

static int renesas_r69328_enable(struct drm_panel *panel)
{
	struct renesas_r69328 *ctx = to_renesas_r69328(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	mipi_dsi_dcs_write_buffer(dsi, address_mode,
				  sizeof(address_mode));

	ret = mipi_dsi_dcs_set_pixel_format(dsi, MIPI_DCS_PIXEL_FMT_24BIT << 4);
	if (ret < 0) {
		dev_err(dev, "Failed to set pixel format: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}

	msleep(100);

	/* MACP Off */
	dsi_generic_write_seq(dsi, R69328_MACP, 0x04);

	dsi_generic_write_seq(dsi, R69328_POWER_SET, 0x14,
			      0x1D, 0x21, 0x67, 0x11, 0x9A);

	dsi_generic_write_seq(dsi, R69328_GAMMA_SET_A, 0x00,
			      0x1A, 0x20, 0x28, 0x25, 0x24,
			      0x26, 0x15, 0x13, 0x11, 0x18,
			      0x1E, 0x1C, 0x00, 0x00, 0x1A,
			      0x20, 0x28, 0x25, 0x24, 0x26,
			      0x15, 0x13, 0x11, 0x18, 0x1E,
			      0x1C, 0x00);
	dsi_generic_write_seq(dsi, R69328_GAMMA_SET_B, 0x00,
			      0x1A, 0x20, 0x28, 0x25, 0x24,
			      0x26, 0x15, 0x13, 0x11, 0x18,
			      0x1E, 0x1C, 0x00, 0x00, 0x1A,
			      0x20, 0x28, 0x25, 0x24, 0x26,
			      0x15, 0x13, 0x11, 0x18, 0x1E,
			      0x1C, 0x00);
	dsi_generic_write_seq(dsi, R69328_GAMMA_SET_C, 0x00,
			      0x1A, 0x20, 0x28, 0x25, 0x24,
			      0x26, 0x15, 0x13, 0x11, 0x18,
			      0x1E, 0x1C, 0x00, 0x00, 0x1A,
			      0x20, 0x28, 0x25, 0x24, 0x26,
			      0x15, 0x13, 0x11, 0x18, 0x1E,
			      0x1C, 0x00);

	/* MACP On */
	dsi_generic_write_seq(dsi, R69328_MACP, 0x03);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}

	msleep(50);

	return 0;
}

static int renesas_r69328_disable(struct drm_panel *panel)
{
	struct renesas_r69328 *ctx = to_renesas_r69328(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}

	msleep(60);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}

	return 0;
}

static int renesas_r69328_unprepare(struct drm_panel *panel)
{
	struct renesas_r69328 *ctx = to_renesas_r69328(panel);

	if (!ctx->prepared)
		return 0;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	usleep_range(5000, 6000);

	regulator_disable(ctx->vddio_supply);
	regulator_disable(ctx->vdd_supply);

	ctx->prepared = false;
	return 0;
}

static const struct drm_display_mode renesas_r69328_mode = {
	.clock = (720 + 92 + 62 + 4) * (1280 + 6 + 3 + 1) * 60 / 1000,
	.hdisplay = 720,
	.hsync_start = 720 + 92,
	.hsync_end = 720 + 92 + 62,
	.htotal = 720 + 92 + 62 + 4,
	.vdisplay = 1280,
	.vsync_start = 1280 + 6,
	.vsync_end = 1280 + 6 + 3,
	.vtotal = 1280 + 6 + 3 + 1,
	.width_mm = 59,
	.height_mm = 105,
};

static int renesas_r69328_get_modes(struct drm_panel *panel,
				   struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &renesas_r69328_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs renesas_r69328_panel_funcs = {
	.prepare = renesas_r69328_prepare,
	.enable = renesas_r69328_enable,
	.disable = renesas_r69328_disable,
	.unprepare = renesas_r69328_unprepare,
	.get_modes = renesas_r69328_get_modes,
};

static int renesas_r69328_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct renesas_r69328 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ctx->vdd_supply))
		return PTR_ERR(ctx->vdd_supply);

	ctx->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->vddio_supply))
		return PTR_ERR(ctx->vddio_supply);

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						  GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		dev_err(dev, "Failed to get reset-gpios: %d\n", ret);
		return ret;
	}

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &renesas_r69328_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void renesas_r69328_remove(struct mipi_dsi_device *dsi)
{
	struct renesas_r69328 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev,
			"Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id renesas_r69328_of_match[] = {
	{ .compatible = "jdi,dx12d100vm0eaa" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, renesas_r69328_of_match);

static struct mipi_dsi_driver renesas_r69328_driver = {
	.probe = renesas_r69328_probe,
	.remove = renesas_r69328_remove,
	.driver = {
		.name = "panel-renesas-r69328",
		.of_match_table = renesas_r69328_of_match,
	},
};
module_mipi_dsi_driver(renesas_r69328_driver);

MODULE_AUTHOR("Maxim Schwalm <maxim.schwalm@gmail.com>");
MODULE_DESCRIPTION("Renesas R69328-based panel driver");
MODULE_LICENSE("GPL");
