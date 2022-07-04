// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Fortemedia FM34NE DSP driver
 */

#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include "dsp-fm34ne.h"

#define FM34NE_I2C_CHECK 0xC0
#define FM34NE_MAX_RETRY 5

enum state {
	FM34NE_BYPASS,
	FM34NE_NS_ENABLE,
	FM34NE_NS_DISABLE,
	FM34NE_MAX,
};

struct fm34ne_dsp_data {
	struct i2c_client *client;

	struct gpio_desc *bypass_gpio;
	struct gpio_desc *reset_gpio;

	struct clk *dap_mclk;
	struct regulator *vdd_supply;

	const struct fm34ne_dsp_devdata *data;
};

static int fm34ne_dsp_write_config(struct i2c_client *client,
				     const u8* config, size_t size)
{
	int ret, i;

	for (i = 0; i < FM34NE_MAX_RETRY; i++) {
		ret = i2c_master_send(client, config, size);
		if (ret > 0)
			return 0;

		msleep(5);
	}

	return ret;
}

static int fm34ne_dsp_set_config(struct fm34ne_dsp_data *fm34, int state)
{
	struct device *dev = &fm34->client->dev;

	const u8 *enable_ns_parameter = fm34->data->enable_noise_suppression;
	int enable_ns_length = fm34->data->enable_ns_length;

	const u8 *disable_ns_parameter = fm34->data->disable_noise_suppression;
	int disable_ns_length = fm34->data->disable_ns_length;

	int ret;

	gpiod_set_value_cansleep(fm34->bypass_gpio, 1);
	msleep(20);

	switch (state) {
	case FM34NE_NS_ENABLE:
		ret = fm34ne_dsp_write_config(fm34->client,
			enable_parameter, sizeof(enable_parameter));
		if (ret < 0) {
			dev_err(dev, "failed to set DSP enable with %d\n", ret);
			goto exit;
		}

		ret = fm34ne_dsp_write_config(fm34->client,
			enable_ns_parameter, enable_ns_length);
		if (ret < 0) {
			dev_err(dev, "failed to enable DSP noise suppression with %d\n", ret);
			goto exit;
		}

		dev_info(dev, "noise suppression enable DSP parameter written\n");
		break;

	case FM34NE_NS_DISABLE:
		ret = fm34ne_dsp_write_config(fm34->client,
			enable_parameter, sizeof(enable_parameter));
		if (ret < 0) {
			dev_err(dev, "failed to set DSP enable with %d\n", ret);
			goto exit;
		}

		ret = fm34ne_dsp_write_config(fm34->client,
			disable_ns_parameter, disable_ns_length);
		if (ret < 0) {
			dev_err(dev, "failed to disable DSP noise suppression with %d\n", ret);
			goto exit;
		}

		dev_info(dev, "noise suppression disable DSP parameter written\n");
		break;

	case FM34NE_BYPASS:
	default:
		ret = fm34ne_dsp_write_config(fm34->client,
			bypass_parameter, sizeof(bypass_parameter));
		if (ret < 0) {
			dev_err(dev, "failed to set DSP bypass with %d\n", ret);
			goto exit;
		}

		dev_info(dev, "bypass DSP parameter written\n");
		break;
	}

exit:
	gpiod_set_value_cansleep(fm34->bypass_gpio, 0);

	return ret;
}

static int fm34ne_dsp_set_hw(struct fm34ne_dsp_data *fm34)
{
	struct device *dev = &fm34->client->dev;
	int ret;

	ret = clk_prepare_enable(fm34->dap_mclk);
	if (ret) {
		dev_err(dev, "failed to enable the DSP MCLK: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(fm34->vdd_supply);
	if (ret < 0) {
		dev_err(dev, "failed to enable vdd power supply\n");
		return ret;
	}

	return 0;
}

static void fm34ne_dsp_reset(struct fm34ne_dsp_data *fm34)
{
	gpiod_set_value_cansleep(fm34->reset_gpio, 1);
	msleep(10);

	gpiod_set_value_cansleep(fm34->reset_gpio, 0);
	msleep(100);
}

static int fm34ne_dsp_init_chip(struct fm34ne_dsp_data *fm34)
{
	const u8 *input_parameter = fm34->data->input_parameter;
	int input_parameter_length = fm34->data->input_parameter_length;
	int ret;

	ret = fm34ne_dsp_set_hw(fm34);
	if (ret)
		return ret;

	fm34ne_dsp_reset(fm34);

	gpiod_set_value_cansleep(fm34->bypass_gpio, 1);
	msleep(20);

	ret = i2c_smbus_write_byte(fm34->client, FM34NE_I2C_CHECK);
	if (ret < 0) {
		dev_info(&fm34->client->dev, "initial write failed\n");
		msleep(50);

		fm34ne_dsp_reset(fm34);
		gpiod_set_value_cansleep(fm34->bypass_gpio, 0);

		return ret;
	}

	ret = fm34ne_dsp_write_config(fm34->client,
		input_parameter, input_parameter_length);
	if (ret < 0)
		return -EINVAL;

	msleep(100);
	gpiod_set_value_cansleep(fm34->bypass_gpio, 0);

	dev_info(&fm34->client->dev, "%s detected\n", fm34->data->model);

	/* Constantly set DSP to bypass mode for now */
	ret = fm34ne_dsp_set_config(fm34, FM34NE_BYPASS);
	if (ret)
		return ret;

	return 0;
}

static int fm34ne_dsp_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct fm34ne_dsp_data *fm34;
	int ret;

	fm34 = devm_kzalloc(dev, sizeof(*fm34), GFP_KERNEL);
	if (!fm34)
		return -ENOMEM;

	i2c_set_clientdata(client, fm34);
	fm34->client = client;

	fm34->dap_mclk = devm_clk_get_optional(dev, "mclk");
	if (IS_ERR(fm34->dap_mclk))
		return dev_err_probe(dev, PTR_ERR(fm34->dap_mclk),
				     "can't retrieve DSP MCLK\n");

	fm34->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(fm34->vdd_supply))
		return dev_err_probe(dev, PTR_ERR(fm34->vdd_supply),
				     "failed to get vdd regulator\n");

	fm34->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						  GPIOD_OUT_LOW);
	if (IS_ERR(fm34->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(fm34->reset_gpio),
				     "failed to get reset GPIO\n");

	/*
	 * Bypass gpio is used to set audio into bypass mode
	 * in relation to dsp to be able to program it. Once
	 * programming is done, bypass gpio has to be set to
	 * low to return dsp into audio processing.
	 */
	fm34->bypass_gpio = devm_gpiod_get_optional(dev, "bypass",
						  GPIOD_OUT_LOW);
	if (IS_ERR(fm34->bypass_gpio))
		return dev_err_probe(dev, PTR_ERR(fm34->bypass_gpio),
				     "failed to get bypass GPIO\n");

	fm34->data = of_device_get_match_data(dev);
	if (!fm34->data)
		return -ENODEV;

	ret = fm34ne_dsp_init_chip(fm34);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to init DSP chip\n");

	return 0;
}

static int fm34ne_dsp_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct fm34ne_dsp_data *fm34 = i2c_get_clientdata(client);

	gpiod_set_value_cansleep(fm34->bypass_gpio, 0);

	regulator_disable(fm34->vdd_supply);

	clk_disable_unprepare(fm34->dap_mclk);

	return 0;
}

static int fm34ne_dsp_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct fm34ne_dsp_data *fm34 = i2c_get_clientdata(client);
	int ret;

	ret = fm34ne_dsp_init_chip(fm34);
	if (ret)
		dev_err(&client->dev, "failed to re-init DSP chip with %d\n", ret);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(fm34ne_dsp_pm_ops,
			 fm34ne_dsp_suspend, fm34ne_dsp_resume);

static const struct fm34ne_dsp_devdata tf101_dsp_data = {
	.model = "ASUS Eee Pad Trnasformer TF101",
	.input_parameter = TF101_input_parameter,
	.input_parameter_length = sizeof(TF101_input_parameter),
	.enable_noise_suppression = TF101_enable_NS,
	.enable_ns_length = sizeof(TF101_enable_NS),
	.disable_noise_suppression = TF101_disable_NS,
	.disable_ns_length = sizeof(TF101_disable_NS),
};

static const struct fm34ne_dsp_devdata tf201_dsp_data = {
	.model = "ASUS Transformer Prime TF201",
	.input_parameter = TF201_input_parameter,
	.input_parameter_length = sizeof(TF201_input_parameter),
	.enable_noise_suppression = TF201_enable_NS,
	.enable_ns_length = sizeof(TF201_enable_NS),
	.disable_noise_suppression = TF201_disable_NS,
	.disable_ns_length = sizeof(TF201_disable_NS),
};

static const struct fm34ne_dsp_devdata tf300t_dsp_data = {
	.model = "ASUS Transformer PAD TF300T",
	.input_parameter = TF300T_input_parameter,
	.input_parameter_length = sizeof(TF300T_input_parameter),
	.enable_noise_suppression = TF201_enable_NS,
	.enable_ns_length = sizeof(TF201_enable_NS),
	.disable_noise_suppression = TF201_disable_NS,
	.disable_ns_length = sizeof(TF201_disable_NS),
};

static const struct fm34ne_dsp_devdata tf700t_dsp_data = {
	.model = "ASUS Transformer Infinity TF700T",
	.input_parameter = TF700T_input_parameter,
	.input_parameter_length = sizeof(TF700T_input_parameter),
	.enable_noise_suppression = TF700T_enable_NS,
	.enable_ns_length = sizeof(TF700T_enable_NS),
	.disable_noise_suppression = TF700T_disable_NS,
	.disable_ns_length = sizeof(TF700T_disable_NS),
};

static const struct fm34ne_dsp_devdata chagall_dsp_data = {
	.model = "Pegatron Chagall",
	.input_parameter = TF300T_input_parameter,
	.input_parameter_length = sizeof(TF300T_input_parameter),
	.enable_noise_suppression = TF201_enable_NS,
	.enable_ns_length = sizeof(TF201_enable_NS),
	.disable_noise_suppression = TF201_disable_NS,
	.disable_ns_length = sizeof(TF201_disable_NS),
};

static const struct of_device_id fm34ne_dsp_match[] = {
	{ .compatible = "asus,tf101-dsp", .data = &tf101_dsp_data },
	{ .compatible = "asus,tf201-dsp", .data = &tf201_dsp_data },
	{ .compatible = "asus,tf300t-dsp", .data = &tf300t_dsp_data },
	{ .compatible = "asus,tf700t-dsp", .data = &tf700t_dsp_data },
	{ .compatible = "pegatron,chagall-dsp", .data = &chagall_dsp_data },
	{ }
};
MODULE_DEVICE_TABLE(of, fm34ne_dsp_match);

static const struct i2c_device_id fm34ne_dsp_id[] = {
	{ "dsp_fm34ne", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fm34ne_dsp_id);

static struct i2c_driver fm34ne_dsp_driver = {
	.driver = {
		.name = "fm34ne-dsp",
		.pm = pm_sleep_ptr(&fm34ne_dsp_pm_ops),
		.of_match_table = fm34ne_dsp_match,
	},
	.probe = fm34ne_dsp_probe,
	.id_table = fm34ne_dsp_id,
};
module_i2c_driver(fm34ne_dsp_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("Fortemedia FM34NE DSP driver");
MODULE_LICENSE("GPL");
