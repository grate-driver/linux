// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016 Golden Delicious Comp. GmbH&Co. KG
 *	Nikolaus Schaller <hns@goldelico.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/iio/iio.h>

#define TSC2007_MEASURE_TEMP0		(0x0 << 4)
#define TSC2007_MEASURE_AUX		(0x2 << 4)
#define TSC2007_MEASURE_TEMP1		(0x4 << 4)
#define TSC2007_SETUP			(0xb << 4)
#define TSC2007_MEASURE_X		(0xc << 4)
#define TSC2007_MEASURE_Y		(0xd << 4)
#define TSC2007_MEASURE_Z1		(0xe << 4)
#define TSC2007_MEASURE_Z2		(0xf << 4)

#define TSC2007_POWER_OFF_IRQ_EN	(0x0 << 2)
#define TSC2007_ADC_ON_IRQ_DIS0		(0x1 << 2)

#define TSC2007_12BIT			(0x0 << 1)
#define MAX_12BIT			((1 << 12) - 1)

#define ADC_ON_12BIT	(TSC2007_12BIT | TSC2007_ADC_ON_IRQ_DIS0)

#define READ_Y		(ADC_ON_12BIT | TSC2007_MEASURE_Y)
#define READ_Z1		(ADC_ON_12BIT | TSC2007_MEASURE_Z1)
#define READ_Z2		(ADC_ON_12BIT | TSC2007_MEASURE_Z2)
#define READ_X		(ADC_ON_12BIT | TSC2007_MEASURE_X)
#define PWRDOWN		(TSC2007_12BIT | TSC2007_POWER_OFF_IRQ_EN)

#define TSC2007_LG_VDEF			1800 /* mV */
#define TSC2007_LG_VMAX			4096 /* mV */
#define TSC2007_LG_APROX		10000

struct tsc2007_adc_devdata {
	struct iio_chan_spec const *channels;
	int num_channels;
};

struct tsc2007_adc {
	struct i2c_client *client;
	struct mutex		mlock;

	u16			x_plate_ohms;

	const struct tsc2007_adc_devdata *data;
};

struct ts_event {
	u16	x;
	u16	y;
	u16	z1, z2;
};

struct lg_adc_graph {
	int x;
	int y;
};

static const struct lg_adc_graph battery_temp_graph[] = {
	/* adc to temperature calibration table */
	{ 1800, -50 }, { 1750, -40 }, { 1680, -30 },
	{ 1585, -20 }, { 1445, -10 }, { 1273,   0 },
	{ 1073,  10 }, {  855,  20 }, {  633,  30 },
	{  498,  40 }, {  366,  50 }, {  290,  60 },
	{  200,  70 }, {  150,  80 }, {  100,  90 },
	{   80, 100 },
};

#define TSC2007_CHAN_IIO(_chan, _name, _type, _chan_info) \
{ \
	.datasheet_name = _name, \
	.type = _type, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |	\
			BIT(_chan_info), \
	.indexed = 1, \
	.channel = _chan, \
}

static const struct iio_chan_spec tsc2007_adc_iio_channel[] = {
	TSC2007_CHAN_IIO(0, "x", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(1, "y", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(2, "z1", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(3, "z2", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(4, "adc", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(5, "rt", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(6, "temp0", IIO_TEMP, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(7, "temp1", IIO_TEMP, IIO_CHAN_INFO_RAW),
};

static const struct iio_chan_spec tsc2007_adc_lg_iio_channel[] = {
	TSC2007_CHAN_IIO(0, "x", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(1, "y", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(2, "z1", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(3, "z2", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(4, "adc", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(5, "rt", IIO_VOLTAGE, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(6, "temp0", IIO_TEMP, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(7, "temp1", IIO_TEMP, IIO_CHAN_INFO_RAW),
	TSC2007_CHAN_IIO(8, "temp3", IIO_TEMP, IIO_CHAN_INFO_RAW),
};

static int tsc2007_adc_xfer(struct tsc2007_adc *tsc2007, u8 cmd)
{
	s32 data;
	u16 val;

	data = i2c_smbus_read_word_data(tsc2007->client, cmd);
	if (data < 0) {
		dev_err(&tsc2007->client->dev, "i2c io error: %d\n", data);
		return data;
	}

	/* The protocol and raw data format from i2c interface:
	 * S Addr Wr [A] Comm [A] S Addr Rd [A] [DataLow] A [DataHigh] NA P
	 * Where DataLow has [D11-D4], DataHigh has [D3-D0 << 4 | Dummy 4bit].
	 */
	val = swab16(data) >> 4;

	dev_dbg(&tsc2007->client->dev, "data: 0x%x, val: 0x%x\n", data, val);

	return val;
}

static u32 tsc2007_adc_calculate_resistance(struct tsc2007_adc *tsc2007, struct ts_event *tc)
{
	u32 rt = 0;

	/* range filtering */
	if (tc->x == MAX_12BIT)
		tc->x = 0;

	if (likely(tc->x && tc->z1)) {
		/* compute touch resistance using equation #1 */
		rt = tc->z2 - tc->z1;
		rt *= tc->x;
		rt *= tsc2007->x_plate_ohms;
		rt /= tc->z1;
		rt = (rt + 2047) >> 12;
	}

	return rt;
}

static int lge_battery_ref(int adc, const struct lg_adc_graph *ref_graph, int size)
{
	int i = 1;
	int slope, const_term;
	int delta_y, delta_x;

	while (adc < ref_graph[i].x && i < (size - 1))
		i++;

	delta_x = ref_graph[i-1].x - ref_graph[i].x;
	delta_y = ref_graph[i-1].y - ref_graph[i].y;

	slope = delta_y * TSC2007_LG_APROX / delta_x;

	const_term = ref_graph[i].y - ref_graph[i].x * slope / TSC2007_LG_APROX;

	return adc * slope / TSC2007_LG_APROX + const_term;
}

static int tsc2007_adc_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct tsc2007_adc *tsc2007 = iio_priv(indio_dev);
	int adc_chan = chan->channel;
	int ret = 0;

	if (adc_chan >= indio_dev->num_channels)
		return -EINVAL;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	mutex_lock(&tsc2007->mlock);

	switch (adc_chan) {
	case 0:
		*val = tsc2007_adc_xfer(tsc2007, READ_X);
		break;
	case 1:
		*val = tsc2007_adc_xfer(tsc2007, READ_Y);
		break;
	case 2:
		*val = tsc2007_adc_xfer(tsc2007, READ_Z1);
		break;
	case 3:
		*val = tsc2007_adc_xfer(tsc2007, READ_Z2);
		break;
	case 4:
		*val = tsc2007_adc_xfer(tsc2007, (ADC_ON_12BIT | TSC2007_MEASURE_AUX));
		break;
	case 5: {
		struct ts_event tc;

		tc.x = tsc2007_adc_xfer(tsc2007, READ_X);
		tc.z1 = tsc2007_adc_xfer(tsc2007, READ_Z1);
		tc.z2 = tsc2007_adc_xfer(tsc2007, READ_Z2);
		*val = tsc2007_adc_calculate_resistance(tsc2007, &tc);
		break;
	}
	case 6:
		*val = tsc2007_adc_xfer(tsc2007,
				    (ADC_ON_12BIT | TSC2007_MEASURE_TEMP0));
		break;
	case 7:
		*val = tsc2007_adc_xfer(tsc2007,
				    (ADC_ON_12BIT | TSC2007_MEASURE_TEMP1));
		break;
	case 8:
		ret = tsc2007_adc_xfer(tsc2007, (ADC_ON_12BIT | TSC2007_MEASURE_AUX));

		ret = (ret * TSC2007_LG_VDEF) / TSC2007_LG_VMAX;

		*val = lge_battery_ref(ret, battery_temp_graph,
				       ARRAY_SIZE(battery_temp_graph));
		break;
	}

	/* Prepare for next touch reading - power down ADC, enable PENIRQ */
	tsc2007_adc_xfer(tsc2007, PWRDOWN);

	mutex_unlock(&tsc2007->mlock);

	ret = IIO_VAL_INT;

	return ret;
}

static const struct iio_info tsc2007_adc_iio_info = {
	.read_raw = tsc2007_adc_read_raw,
};

static int tsc2007_adc_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct tsc2007_adc *tsc2007;
	struct iio_dev *indio_dev;
	u32 val;
	int err;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*tsc2007));
	if (!indio_dev)
		return -ENOMEM;

	tsc2007 = iio_priv(indio_dev);
	tsc2007->client = client;

	tsc2007->data = of_device_get_match_data(&client->dev);
	if (!tsc2007->data)
		return -ENODEV;

	indio_dev->name = dev_name(&client->dev);
	indio_dev->info = &tsc2007_adc_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = tsc2007->data->channels;
	indio_dev->num_channels = tsc2007->data->num_channels;

	mutex_init(&tsc2007->mlock);

	if (!device_property_read_u32(&client->dev, "ti,x-plate-ohms", &val))
		tsc2007->x_plate_ohms = val;
	else
		tsc2007->x_plate_ohms = 1;

	/* power down the chip (TSC2007_SETUP does not ACK on I2C) */
	err = tsc2007_adc_xfer(tsc2007, PWRDOWN);
	if (err < 0) {
		dev_err(&client->dev,
			"Failed to setup chip: %d\n", err);
		return err;	/* chip does not respond */
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct tsc2007_adc_devdata lge_x3_tsc2007_data = {
	.channels = tsc2007_adc_lg_iio_channel,
	.num_channels = ARRAY_SIZE(tsc2007_adc_lg_iio_channel),
};

static const struct tsc2007_adc_devdata tsc2007_data = {
	.channels = tsc2007_adc_iio_channel,
	.num_channels = ARRAY_SIZE(tsc2007_adc_iio_channel),
};

static const struct i2c_device_id tsc2007_adc_id_table[] = {
	{ "tsc2007_adc", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tsc2007_adc_id_table);

static const struct of_device_id tsc2007_adc_of_match[] = {
	{ .compatible = "ti,tsc2007-adc", .data = &tsc2007_data },
	{ .compatible = "lge-ti,tsc2007-adc", .data = &lge_x3_tsc2007_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tsc2007_adc_of_match);

static struct i2c_driver tsc2007_adc_driver = {
	.driver = {
		.name	= "tsc2007_adc",
		.of_match_table = tsc2007_adc_of_match,
	},
	.id_table	= tsc2007_adc_id_table,
	.probe		= tsc2007_adc_probe,
};
module_i2c_driver(tsc2007_adc_driver);

MODULE_AUTHOR("Kwangwoo Lee <kwlee@mtekvision.com>");
MODULE_DESCRIPTION("TSC2007 ADC Driver");
MODULE_LICENSE("GPL");
