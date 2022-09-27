// SPDX-License-Identifier: GPL-2.0-only
/*
 * AL3000a - Dyna Image Ambient Light Sensor
 */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define AL3000A_REG_SYSTEM		0x00
#define AL3000A_REG_DATA		0x05

#define AL3000A_CONFIG_ENABLE		0x00
#define AL3000A_CONFIG_DISABLE		0x0B
#define AL3000A_CONFIG_RESET		0x0F

static int lux_table[64] = {
	    1,     1,     1,     2,     2,     2,     3,      4,
	    4,     5,    10,    20,    50,    70,   100,    150,
	  200,   250,   300,   350,   400,   500,   600,    700,
	  900,  1100,  1400,  1500,  1500,  1500,  1500,   1500,
	 1500,  1500,  1500,  1500,  1500,  1500,  1500,   1500,
	 1500,  1795,  2154,  2586,  3105,  3728,  4475,   5372,
	 6449,  7743,  9295, 11159, 13396, 16082, 19307,  23178,
	27826, 33405, 40103, 48144, 57797, 69386, 83298, 100000
};

struct al3000a_data {
	struct i2c_client *client;
};

static const struct iio_chan_spec al3000a_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	}
};

static int al3000a_set_pwr(struct i2c_client *client, bool pwr)
{
	u8 val = AL3000A_CONFIG_DISABLE;

	if (pwr)
		val = AL3000A_CONFIG_ENABLE;	

	return i2c_smbus_write_byte_data(client, AL3000A_REG_SYSTEM, val);
}

static void al3000a_set_pwr_off(void *_data)
{
	struct al3000a_data *data = _data;

	al3000a_set_pwr(data->client, false);
}

static int al3000a_init(struct al3000a_data *data)
{
	int ret;

	ret = al3000a_set_pwr(data->client, true);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_write_byte_data(data->client, AL3000A_REG_SYSTEM,
					AL3000A_CONFIG_RESET);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_write_byte_data(data->client, AL3000A_REG_SYSTEM,
					AL3000A_CONFIG_ENABLE);
	if (ret < 0)
		return ret;

	return 0;
}

static int al3000a_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan, int *val,
			   int *val2, long mask)
{
	struct al3000a_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = i2c_smbus_read_byte_data(data->client,
					       AL3000A_REG_DATA);
		if (ret < 0)
			return ret;

		*val = lux_table[ret & 0x3F];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 1;
		return IIO_VAL_INT;
	}
	return -EINVAL;
}

static const struct iio_info al3000a_info = {
	.read_raw	= al3000a_read_raw,
};

static int al3000a_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct al3000a_data *data;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;

	indio_dev->info = &al3000a_info;
	indio_dev->name = "al3000a";
	indio_dev->channels = al3000a_channels;
	indio_dev->num_channels = ARRAY_SIZE(al3000a_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = al3000a_init(data);
	if (ret < 0) {
		dev_err(&client->dev, "al3000a chip init failed\n");
		return ret;
	}

	ret = devm_add_action_or_reset(&client->dev,
					al3000a_set_pwr_off,
					data);
	if (ret < 0)
		return ret;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int __maybe_unused al3000a_suspend(struct device *dev)
{
	return al3000a_set_pwr(to_i2c_client(dev), false);
}

static int __maybe_unused al3000a_resume(struct device *dev)
{
	return al3000a_set_pwr(to_i2c_client(dev), true);
}
static SIMPLE_DEV_PM_OPS(al3000a_pm_ops, al3000a_suspend, al3000a_resume);

static const struct i2c_device_id al3000a_id[] = {
	{ "al3000a", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, al3000a_id);

static const struct of_device_id al3000a_of_match[] = {
	{ .compatible = "dynaimage,al3000a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, al3000a_of_match);

static struct i2c_driver al3000a_driver = {
	.driver = {
		.name = "al3000a",
		.of_match_table = al3000a_of_match,
		.pm = &al3000a_pm_ops,
	},
	.probe		= al3000a_probe,
	.id_table	= al3000a_id,
};
module_i2c_driver(al3000a_driver);

MODULE_AUTHOR("Svyatolsav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("al3000a Ambient Light Sensor driver");
MODULE_LICENSE("GPL v2");
