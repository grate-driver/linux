// SPDX-License-Identifier: GPL-2.0
/*
 * Solomon SSD2825 DSI to LVDS bridge driver
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <drm/drm_drv.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <video/mipi_display.h>
#include <video/videomode.h>

#include "ssd2825.h"

struct ssd2825_dsi_output {
	struct mipi_dsi_device *dev;
	struct drm_panel *panel;
	struct drm_bridge *bridge;
};

struct ssd2825_priv {
	struct spi_device *spi;
	struct device *dev;

	struct gpio_desc *power_gpio;
	struct gpio_desc *reset_gpio;

	struct clk *tx_clk;

	int enabled;

	struct mipi_dsi_host dsi_host;
	struct drm_bridge bridge;
	struct ssd2825_dsi_output output;

	u32 pd_lines;		/* number of Parallel Port Input Data Lines */
	u32 dsi_lanes;		/* number of DSI Lanes */

	/* Parameters for PLL programming */
	u32 pll_freq_kbps;	/* PLL in kbps */
	u32 nibble_freq_khz;	/* PLL div by 4 */

	u32 hzd;		/* HS Zero Delay in ns*/
	u32 hpd;		/* HS Prepare Delay is ns */
};

static inline struct ssd2825_priv *dsi_host_to_ssd2825(struct mipi_dsi_host
							 *host)
{
	return container_of(host, struct ssd2825_priv, dsi_host);
}

static inline struct ssd2825_priv *bridge_to_ssd2825(struct drm_bridge
						     *bridge)
{
	return container_of(bridge, struct ssd2825_priv, bridge);
}

static int ssd2825_write_raw(struct ssd2825_priv *priv, u8 high_byte, u8 low_byte)
{
	struct spi_device *spi = priv->spi;
	struct spi_message msg;
	struct spi_transfer xfer;
	u8 tx_buf[2];
	int ret;

	memset(&xfer, 0, sizeof(xfer));

	/* low byte is command, high byte is determinator */
	tx_buf[0] = low_byte;
	tx_buf[1] = high_byte;

	xfer.tx_buf = tx_buf;
	xfer.bits_per_word = 9;
	xfer.len = 2;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	ret = spi_sync(spi, &msg);
	if (ret)
		dev_err(&spi->dev, "command: %x, spi error: %d\n", low_byte, ret);

	return ret;
}

static int ssd2825_write_reg(struct ssd2825_priv *priv, u8 reg, u16 command)
{
	u8 cmd1, cmd2;
	int ret;

	/* send low byte first and then high byte */
	cmd1 = (command & 0x00FF);
	cmd2 = (command & 0xFF00) >> 8;

	ret = ssd2825_write_raw(priv, SSD2825_REG_BYTE, reg);
	if (ret)
		return ret;

	ret = ssd2825_write_raw(priv, SSD2825_CMD_BYTE, cmd1);
	if (ret)
		return ret;

	ret = ssd2825_write_raw(priv, SSD2825_CMD_BYTE, cmd2);
	if (ret)
		return ret;

	return 0;
}

static int ssd2825_write_dsi(struct ssd2825_priv *priv, const u8 *command, int len)
{
	int ret, i;

	ret = ssd2825_write_reg(priv, SSD2825_PACKET_SIZE_CTRL_REG_1, len);
	if (ret)
		return ret;

	ret = ssd2825_write_raw(priv, SSD2825_REG_BYTE, SSD2825_PACKET_DROP_REG);
	if (ret)
		return ret;

	for (i = 0; i < len; i++) {
		ret = ssd2825_write_raw(priv, SSD2825_CMD_BYTE, command[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int ssd2825_read_raw(struct ssd2825_priv *priv, u8 cmd, u16 *data)
{
	struct spi_device *spi = priv->spi;
	struct spi_message  msg;
	struct spi_transfer xfer[2];
	u8 tx_buf[2];
	u8 rx_buf[2];
	int ret;
	u16 rxtmp0, rxtmp1;

	memset(&xfer, 0, sizeof(xfer));

	tx_buf[1] = (cmd & 0xFF00) >> 8;
	tx_buf[0] = (cmd & 0x00FF);

	xfer[0].tx_buf = tx_buf;
	xfer[0].bits_per_word = 9;
	xfer[0].len = 2;

	xfer[1].rx_buf = rx_buf;
	xfer[1].bits_per_word = 16;
	xfer[1].len = 2;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer[0], &msg);
	spi_message_add_tail(&xfer[1], &msg);

	ret = spi_sync(spi, &msg);
	if (ret)
		dev_err(&spi->dev, "spi_sync_read failed %d\n", ret);

	rxtmp0 = (u16) rx_buf[0];
	rxtmp1 = (u16) rx_buf[1];

	*data = (rxtmp1 | (rxtmp0 << 8));

	return ret;
}

static int ssd2825_read_reg(struct ssd2825_priv *priv, u8 reg, u16 *data)
{
	int ret;

	ret = ssd2825_write_reg(priv, SSD2825_SPI_READ_REG, SSD2825_SPI_READ_REG_RESET);
	if (ret)
		return ret;

	ret = ssd2825_write_raw(priv, SSD2825_REG_BYTE, reg);
	if (ret)
		return ret;

	ret = ssd2825_read_raw(priv, SSD2825_SPI_READ_REG_RESET, data);
	if (ret)
		return ret;

	return 0;
}

static int ssd2825_dsi_host_attach(struct mipi_dsi_host *host,
				    struct mipi_dsi_device *dev)
{
	struct ssd2825_priv *priv = dsi_host_to_ssd2825(host);
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	struct device_node *ep;
	int ret;

	if (dev->lanes > 4) {
		dev_err(priv->dev, "unsupported number of data lanes(%u)\n",
			dev->lanes);
		return -EINVAL;
	}

	/*
	 * ssd2825 supports both Video and Pulse mode, but the driver only
	 * implements Video (event) mode currently
	 */
	if (!(dev->mode_flags & MIPI_DSI_MODE_VIDEO)) {
		dev_err(priv->dev, "Only MIPI_DSI_MODE_VIDEO is supported\n");
		return -EOPNOTSUPP;
	}

	ret = drm_of_find_panel_or_bridge(host->dev->of_node, 1, 0, &panel,
					  &bridge);
	if (ret)
		return ret;

	if (panel) {
		bridge = drm_panel_bridge_add_typed(panel,
						    DRM_MODE_CONNECTOR_DSI);
		if (IS_ERR(bridge))
			return PTR_ERR(bridge);
	}

	priv->output.dev = dev;
	priv->output.bridge = bridge;
	priv->output.panel = panel;

	priv->dsi_lanes = dev->lanes;

	/* get input ep (port0/endpoint0) */
	ret = -EINVAL;
	ep = of_graph_get_endpoint_by_regs(host->dev->of_node, 0, 0);
	if (ep) {
		ret = of_property_read_u32(ep, "data-lines", &priv->pd_lines);

		of_node_put(ep);
	}

	if (ret)
		priv->pd_lines = mipi_dsi_pixel_format_to_bpp(dev->format);

	drm_bridge_add(&priv->bridge);

	return 0;
}

static int ssd2825_dsi_host_detach(struct mipi_dsi_host *host,
				   struct mipi_dsi_device *dev)
{
	struct ssd2825_priv *priv = dsi_host_to_ssd2825(host);

	drm_bridge_remove(&priv->bridge);
	if (priv->output.panel)
		drm_panel_bridge_remove(priv->output.bridge);

	return 0;
}

static ssize_t ssd2825_dsi_host_transfer(struct mipi_dsi_host *host,
					 const struct mipi_dsi_msg *msg)
{
	struct ssd2825_priv *priv = dsi_host_to_ssd2825(host);
	u8 buf = *(u8 *)msg->tx_buf;
	u16 config;
	int ret;

	if (!priv->enabled) {
		dev_err(priv->dev, "Bridge is not enabled\n");
		return -ENODEV;
	}

	if (msg->rx_len) {
		dev_warn(priv->dev, "MIPI rx is not supported\n");
		return -EOPNOTSUPP;
	}

	/* Read config register value to manipulate it further */
	ret = ssd2825_read_reg(priv, SSD2825_CONFIGURATION_REG, &config);
	if (ret)
		return ret;

	switch (msg->type) {
	case MIPI_DSI_DCS_SHORT_WRITE:
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
	case MIPI_DSI_DCS_LONG_WRITE:
		config |= SSD2825_CONF_REG_DCS;
		break;
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
	case MIPI_DSI_GENERIC_LONG_WRITE:
		config &= ~SSD2825_CONF_REG_DCS;
		break;
	case MIPI_DSI_DCS_READ:
	case MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM:
	default:
		// no reading for now
		return 0;
	}

	ret = ssd2825_write_reg(priv, SSD2825_CONFIGURATION_REG, config);
	if (ret)
		return ret;

	ret = ssd2825_write_reg(priv, SSD2825_VC_CTRL_REG, 0x0000);
	if (ret)
		return ret;

	ret = ssd2825_write_dsi(priv, msg->tx_buf, msg->tx_len);
	if (ret)
		return ret;

	if (buf == MIPI_DCS_SET_DISPLAY_ON) {
		ssd2825_write_reg(priv, SSD2825_CONFIGURATION_REG,
				  SSD2825_CONF_REG_HS | SSD2825_CONF_REG_VEN |
				  SSD2825_CONF_REG_DCS | SSD2825_CONF_REG_ECD |
				  SSD2825_CONF_REG_EOT);
		ssd2825_write_reg(priv, SSD2825_PLL_CTRL_REG, 0x0001);
		ssd2825_write_reg(priv, SSD2825_VC_CTRL_REG, 0x0000);
	}

	return 0;
}

static const struct mipi_dsi_host_ops ssd2825_dsi_host_ops = {
	.attach = ssd2825_dsi_host_attach,
	.detach = ssd2825_dsi_host_detach,
	.transfer = ssd2825_dsi_host_transfer,
};

static void ssd2825_hw_reset(struct ssd2825_priv *priv)
{
	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(priv->reset_gpio, 0);
	usleep_range(5000, 6000);
}

/*
 * PLL configuration register settings.
 *
 * See the "PLL Configuration Register Description" in the SSD2825 datasheet.
 */
static u16 construct_pll_config(struct ssd2825_priv *priv,
				u32 desired_pll_freq_kbps, u32 reference_freq_khz)
{
	u32 div_factor = 1, mul_factor, fr = 0;

	while (reference_freq_khz / (div_factor + 1) >= SSD2825_REF_MIN_CLK)
		div_factor++;
	if (div_factor > 31)
		div_factor = 31;

	mul_factor = DIV_ROUND_UP(desired_pll_freq_kbps * div_factor,
				  reference_freq_khz);

	priv->pll_freq_kbps = reference_freq_khz * mul_factor / div_factor;
	priv->nibble_freq_khz = priv->pll_freq_kbps / 4;

	if (priv->pll_freq_kbps >= 501000)
		fr = 3;
	else if (priv->pll_freq_kbps >= 251000)
		fr = 2;
	else if (priv->pll_freq_kbps >= 126000)
		fr = 1;

	return (fr << 14) | (div_factor << 8) | mul_factor;
}

static u32 ssd2825_to_ns(u32 khz)
{
	return (1000 * 1000 / khz);
}

static int ssd2825_setup_pll(struct ssd2825_priv *priv,
			      const struct drm_display_mode *mode)
{
	u16 pll_config, lp_div;
	u32 nibble_delay, pclk_mult, tx_freq_khz;
	u8 hzd, hpd;

	tx_freq_khz = clk_get_rate(priv->tx_clk) / 1000;
	pclk_mult = priv->pd_lines / priv->dsi_lanes + 1;
	pll_config = construct_pll_config(priv, pclk_mult * mode->clock,
					  tx_freq_khz);

	lp_div = priv->pll_freq_kbps / (SSD2825_LP_MIN_CLK * 8);

	nibble_delay = ssd2825_to_ns(priv->nibble_freq_khz);

	hzd = priv->hzd / nibble_delay;
	hpd = (priv->hpd - 4 * nibble_delay) / nibble_delay;

	/* Disable PLL */
	ssd2825_write_reg(priv, SSD2825_PLL_CTRL_REG, 0x0000);
	ssd2825_write_reg(priv, SSD2825_LINE_CTRL_REG, 0x0001);

	/* Set delays */
	dev_info(priv->dev, "SSD2825_DELAY_ADJ_REG_1 0x%x\n", (hzd << 8) | hpd);
	ssd2825_write_reg(priv, SSD2825_DELAY_ADJ_REG_1, (hzd << 8) | hpd);

	/* Set PLL coeficients */
	dev_info(priv->dev, "SSD2825_PLL_CONFIGURATION_REG 0x%x\n", pll_config);
	ssd2825_write_reg(priv, SSD2825_PLL_CONFIGURATION_REG, pll_config);

	/* Clock Control Register */
	dev_info(priv->dev, "SSD2825_CLOCK_CTRL_REG 0x%x\n",
		 SSD2828_LP_CLOCK_DIVIDER(lp_div));
	ssd2825_write_reg(priv, SSD2825_CLOCK_CTRL_REG,
			  SSD2828_LP_CLOCK_DIVIDER(lp_div));

	/* Enable PLL */
	ssd2825_write_reg(priv, SSD2825_PLL_CTRL_REG, 0x0001);
	ssd2825_write_reg(priv, SSD2825_VC_CTRL_REG, 0);

	return 0;
}
static void ssd2825_bridge_pre_enable(struct drm_bridge *bridge)
{
	struct ssd2825_priv *priv = bridge_to_ssd2825(bridge);
	int ret;

	if (priv->enabled)
		return;

	/* Power Sequence */
	ret = clk_prepare_enable(priv->tx_clk);
	if (ret < 0)
		dev_err(priv->dev, "error enabling tx_clk (%d)\n", ret);

	gpiod_set_value_cansleep(priv->power_gpio, 1);
	usleep_range(1000, 2000);

	ssd2825_hw_reset(priv);

	priv->enabled = true;
}

static void ssd2825_bridge_enable(struct drm_bridge *bridge)
{
	struct ssd2825_priv *priv = bridge_to_ssd2825(bridge);
	struct mipi_dsi_device *dsi_dev = priv->output.dev;
	unsigned long mode_flags = dsi_dev->mode_flags;
	const struct drm_display_mode *mode =
		&bridge->encoder->crtc->state->adjusted_mode;
	struct device *dev = priv->dev;
	u8 pixel_format;

	if (mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS) {
		dev_warn_once(dev,
			      "Non-continuous mode unimplemented, falling back to continuous\n");
		mode_flags &= ~MIPI_DSI_CLOCK_NON_CONTINUOUS;
	}

	if (mode_flags & MIPI_DSI_MODE_VIDEO_BURST) {
		dev_warn_once(dev,
			      "Burst mode unimplemented, falling back to simple\n");
		mode_flags &= ~MIPI_DSI_MODE_VIDEO_BURST;
	}

	/* Perform SW reset */
	ssd2825_write_reg(priv, SSD2825_OPERATION_CTRL_REG, 0x0100);

	switch (dsi_dev->format) {
	case MIPI_DSI_FMT_RGB565:
		pixel_format = 0x00;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		pixel_format = 0x01;
		break;
	case MIPI_DSI_FMT_RGB666:
		pixel_format = 0x02;
		break;
	case MIPI_DSI_FMT_RGB888:
	default:
		pixel_format = 0x03;
		break;
	}

	/* Set panel timings */
	ssd2825_write_reg(priv, SSD2825_RGB_INTERFACE_CTRL_REG_1,
			 ((mode->vtotal - mode->vsync_end) << 8) |
			 (mode->htotal - mode->hsync_end));
	ssd2825_write_reg(priv, SSD2825_RGB_INTERFACE_CTRL_REG_2,
			 ((mode->vtotal - mode->vsync_start) << 8) |
			 (mode->htotal - mode->hsync_start));
	ssd2825_write_reg(priv, SSD2825_RGB_INTERFACE_CTRL_REG_3,
			 ((mode->vsync_start - mode->vdisplay) << 8) |
			 (mode->hsync_start - mode->hdisplay));
	ssd2825_write_reg(priv, SSD2825_RGB_INTERFACE_CTRL_REG_4, mode->hdisplay);
	ssd2825_write_reg(priv, SSD2825_RGB_INTERFACE_CTRL_REG_5, mode->vdisplay);
	ssd2825_write_reg(priv, SSD2825_RGB_INTERFACE_CTRL_REG_6,
			  SSD2825_HSYNC_HIGH | SSD2825_VSYNC_HIGH |
			  SSD2825_PCKL_HIGH | SSD2825_NON_BURST |
			  pixel_format);

	ssd2825_write_reg(priv, SSD2825_LANE_CONFIGURATION_REG, dsi_dev->lanes - 1);
	ssd2825_write_reg(priv, SSD2825_TEST_REG, 0x0004);

	/* Call PLL configuration */
	ssd2825_setup_pll(priv, mode);

	usleep_range(10000, 11000);

	/* Initial DSI configuration register set */
	ssd2825_write_reg(priv, SSD2825_CONFIGURATION_REG,
			  SSD2825_CONF_REG_CKE | SSD2825_CONF_REG_DCS |
			  SSD2825_CONF_REG_ECD | SSD2825_CONF_REG_EOT);
	ssd2825_write_reg(priv, SSD2825_VC_CTRL_REG, 0);
}

static void ssd2825_bridge_disable(struct drm_bridge *bridge)
{
	struct ssd2825_priv *priv = bridge_to_ssd2825(bridge);

	if (!priv->enabled)
		return;

	msleep(100);

	/* Exit DSI configuration register set */
	ssd2825_write_reg(priv, SSD2825_CONFIGURATION_REG,
			  SSD2825_CONF_REG_ECD | SSD2825_CONF_REG_EOT);
	ssd2825_write_reg(priv, SSD2825_VC_CTRL_REG, 0);

	/* HW disable */
	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(priv->power_gpio, 0);

	clk_disable_unprepare(priv->tx_clk);

	priv->enabled = false;
}

static int ssd2825_bridge_attach(struct drm_bridge *bridge,
				 enum drm_bridge_attach_flags flags)
{
	struct ssd2825_priv *priv = bridge_to_ssd2825(bridge);

	return drm_bridge_attach(bridge->encoder, priv->output.bridge, bridge,
				 flags);
}

static enum drm_mode_status
ssd2825_bridge_mode_valid(struct drm_bridge *bridge,
			  const struct drm_display_info *info,
			  const struct drm_display_mode *mode)
{
	return MODE_OK;
}

static const struct drm_bridge_funcs ssd2825_bridge_funcs = {
	.attach = ssd2825_bridge_attach,
	.mode_valid = ssd2825_bridge_mode_valid,
	.pre_enable = ssd2825_bridge_pre_enable,
	.enable = ssd2825_bridge_enable,
	.disable = ssd2825_bridge_disable,
};

static int ssd2825_probe(struct spi_device *spi)
{
	struct ssd2825_priv *priv;
	struct device *dev = &spi->dev;
	struct device_node *np = dev->of_node;
	int ret;

	spi->bits_per_word = 9;
	spi->mode = SPI_MODE_3;

	ret = spi_setup(spi);
	if (ret)
		return ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	spi_set_drvdata(spi, priv);
	priv->spi = spi;

	dev_set_drvdata(dev, priv);
	priv->dev = dev;

	priv->tx_clk = devm_clk_get_optional(dev, "tx_clk");
	if (IS_ERR(priv->tx_clk))
		return dev_err_probe(dev, PTR_ERR(priv->tx_clk),
				     "can't retrieve bridge tx_clk\n");

	priv->power_gpio = devm_gpiod_get_optional(dev, "power",
						   GPIOD_OUT_LOW);
	if (IS_ERR(priv->power_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->power_gpio),
				     "failed to get power GPIO\n");

	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
				     "failed to get reset GPIO\n");

	device_property_read_u32(dev, "solomon,hs-zero-delay", &priv->hzd);
	device_property_read_u32(dev, "solomon,hs-prep-delay", &priv->hpd);

	priv->dsi_host.dev = dev;
	priv->dsi_host.ops = &ssd2825_dsi_host_ops;

	priv->bridge.funcs = &ssd2825_bridge_funcs;
	priv->bridge.of_node = np;

	return mipi_dsi_host_register(&priv->dsi_host);
}

static void ssd2825_remove(struct spi_device *spi)
{
	struct ssd2825_priv *priv = spi_get_drvdata(spi);

	mipi_dsi_host_unregister(&priv->dsi_host);
}

static const struct spi_device_id ssd2825_id[] = {
	{ "ssd2825", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, ssd2825_id);

static const struct of_device_id ssd2825_of_match[] = {
	{ .compatible = "solomon,ssd2825", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ssd2825_of_match);

static struct spi_driver ssd2825_driver = {
	.driver = {
		.name = "ssd2825",
		.of_match_table = ssd2825_of_match,
	},
	.probe = ssd2825_probe,
	.remove = ssd2825_remove,
	.id_table = ssd2825_id,
};
module_spi_driver(ssd2825_driver);

MODULE_DESCRIPTION("Solomon SSD2825 RGB to MIPI-DSI bridge driver SPI");
MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_LICENSE("GPL");
