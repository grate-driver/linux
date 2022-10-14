// SPDX-License-Identifier: GPL-2.0
/*
 * CSR GSD5T GNSS NMEA chip driver
 */
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/gnss.h>
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/serdev.h>

#include "serial.h"

struct csr_data {
	struct device *dev;

	struct gpio_desc *power_gpio;
	struct gpio_desc *reset_gpio;

	struct clk *ref_clk;

	struct regulator *vcc;
	struct regulator *vio;
};

static int csr_set_active(struct gnss_serial *gserial)
{
	struct csr_data *data = gnss_serial_get_drvdata(gserial);
	int ret;

	ret = regulator_enable(data->vcc);
	if (ret)
		return ret;

	ret = regulator_enable(data->vio);
	if (ret)
		return ret;

	ret = clk_prepare_enable(data->ref_clk);
	if (ret < 0)
		return ret;

	gpiod_set_value_cansleep(data->power_gpio, 1);
	usleep_range(10000, 11000);

	gpiod_set_value_cansleep(data->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(data->reset_gpio, 0);

	return 0;
}

static int csr_set_standby(struct gnss_serial *gserial)
{
	struct csr_data *data = gnss_serial_get_drvdata(gserial);
	int ret;

	gpiod_set_value_cansleep(data->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(data->power_gpio, 0);

	clk_disable_unprepare(data->ref_clk);

	ret = regulator_disable(data->vio);
	if (ret)
		return ret;

	ret = regulator_disable(data->vcc);
	if (ret)
		return ret;

	return 0;
}

static int csr_set_power(struct gnss_serial *gserial,
			 enum gnss_serial_pm_state state)
{
	switch (state) {
	case GNSS_SERIAL_ACTIVE:
		return csr_set_active(gserial);
	case GNSS_SERIAL_OFF:
	case GNSS_SERIAL_STANDBY:
		return csr_set_standby(gserial);
	}

	return -EINVAL;
}

static const struct gnss_serial_ops csr_gserial_ops = {
	.set_power = csr_set_power,
};

static int csr_probe(struct serdev_device *serdev)
{
	struct gnss_serial *gserial;
	struct csr_data *data;
	int ret;

	gserial = gnss_serial_allocate(serdev, sizeof(*data));
	if (IS_ERR(gserial))
		return dev_err_probe(data->dev, PTR_ERR(gserial),
				     "can't allocate gnss serial\n");

	gserial->ops = &csr_gserial_ops;
	gserial->gdev->type = GNSS_TYPE_NMEA;

	data = gnss_serial_get_drvdata(gserial);

	data->dev = &serdev->dev;

	data->vcc = devm_regulator_get_optional(data->dev, "vcc");
	if (IS_ERR(data->vcc)) {
		ret = PTR_ERR(data->vcc);
		dev_err(data->dev,
			"failed to get vcc regulator: %d\n", ret);
		goto err_free_gserial;
	}

	data->vio = devm_regulator_get_optional(data->dev, "vio");
	if (IS_ERR(data->vio)) {
		ret = PTR_ERR(data->vio);
		dev_err(data->dev,
			"failed to get vio regulator: %d\n", ret);
		goto err_free_gserial;
	}

	data->power_gpio = devm_gpiod_get_optional(data->dev, "power",
						   GPIOD_OUT_LOW);
	if (IS_ERR(data->power_gpio)) {
		ret = PTR_ERR(data->power_gpio);
		dev_err(data->dev,
			"failed to get power gpio: %d\n", ret);
		goto err_free_gserial;
	}

	data->reset_gpio = devm_gpiod_get_optional(data->dev, "reset",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(data->reset_gpio)) {
		ret = PTR_ERR(data->reset_gpio);
		dev_err(data->dev,
			"failed to get reset gpio: %d\n", ret);
		goto err_free_gserial;
	}

	data->ref_clk = devm_clk_get_optional(data->dev, "ref_clk");
	if (IS_ERR(data->ref_clk)) {
		ret = PTR_ERR(data->ref_clk);
		dev_err(data->dev,
			"can't retrieve gnss ref_clk: %d\n", ret);
		goto err_free_gserial;
	}

	ret = gnss_serial_register(gserial);
	if (ret)
		goto err_free_gserial;

	dev_info(data->dev, "CSR GSD5T probed\n");

	return 0;

err_free_gserial:
	gnss_serial_free(gserial);

	return ret;
}

static void csr_remove(struct serdev_device *serdev)
{
	struct gnss_serial *gserial = serdev_device_get_drvdata(serdev);

	gnss_serial_deregister(gserial);
	csr_set_standby(gserial);
	gnss_serial_free(gserial);
}

static const struct of_device_id csr_of_match[] = {
	{ .compatible = "csr,gsd5t" },
	{ },
};
MODULE_DEVICE_TABLE(of, csr_of_match);

static struct serdev_device_driver csr_driver = {
	.driver	= {
		.name		= "gnss-csr",
		.of_match_table	= csr_of_match,
		.pm		= &gnss_serial_pm_ops,
	},
	.probe	= csr_probe,
	.remove	= csr_remove,
};
module_serdev_device_driver(csr_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("CSR GNSS receiver driver");
MODULE_LICENSE("GPL");
