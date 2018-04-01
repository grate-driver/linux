// SPDX-License-Identifier: GPL-2.0-or-later
/*  drivers/misc/sec_jack.c
 *
 *  Copyright (C) 2010 Samsung Electronics Co.Ltd
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/extcon-provider.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include <linux/iio/consumer.h>


#define MAX_ZONE_LIMIT		10
#define DEBOUNCE_TIME	150		/* 150ms */
#define WAKE_LOCK_TIME		(5000)	/* 5 sec */
#define NUM_INPUT_DEVICE_ID	1


enum {
	SEC_JACK_NO_DEVICE		= 0,
	SEC_HEADSET_4POLE		= BIT(0),
	SEC_HEADSET_3POLE		= BIT(1),
	SEC_TTY_DEVICE			= BIT(2),
	SEC_FM_HEADSET			= BIT(3),
	SEC_FM_SPEAKER			= BIT(4),
	SEC_TVOUT_DEVICE		= BIT(5),
	SEC_EXTRA_DOCK_SPEAKER		= BIT(6),
	SEC_EXTRA_CAR_DOCK_SPEAKER	= BIT(7),
	SEC_UNKNOWN_DEVICE		= BIT(8),
};


struct jack_detect_zone {
	unsigned int adc_high;
	unsigned int delay_ms;
	unsigned int check_count;
	unsigned int jack_type;
};

struct jack_button_zone {
	unsigned int code;
	unsigned int adc_low;
	unsigned int adc_high;
};

struct jack_platform_data {
	struct platform_device *pdev;
	struct jack_detect_zone	*zones;
	struct jack_button_zone	*buttons_zones;
	int	num_zones;
	int	num_buttons_zones;
	struct gpio_desc *detect_gpio;
	struct gpio_desc *ear_micbias_gpio;
	bool det_active_high;
};

struct jack_info {
	struct jack_platform_data *pdata;

	struct delayed_work jack_detect_work;
	struct work_struct buttons_work;
	struct workqueue_struct *queue;
	struct wakeup_source det_wakeup_source;

	struct jack_detect_zone *zone;

	struct input_dev *input_dev;
	struct input_handler handler;
	struct input_handle handle;
	struct input_device_id ids[NUM_INPUT_DEVICE_ID];
	int detect_irq;
	int pressed;
	int pressed_code;
	unsigned int cur_jack_type;

	struct iio_channel *adc_channel;

	struct extcon_dev *switch_jack_detection;
};


static const unsigned int jack_cables[] = {
	EXTCON_JACK_HEADPHONE,
};


static int p4wifi_jack_read_adc(struct jack_info *info);


static bool p4wifi_jack_buttons_filter(struct input_handle *handle,
				    unsigned int type, unsigned int code,
				    int value)
{
	struct jack_info *info = handle->handler->private;

	if (type != EV_KEY || code != KEY_UNKNOWN)
		return false;

	info->pressed = value;
	queue_work(info->queue, &info->buttons_work);

	return true;
}

static int p4wifi_jack_buttons_connect(struct input_handler *handler,
				    struct input_dev *input_dev,
				    const struct input_device_id *id)
{
	struct jack_info *info = handler->private;
	struct jack_platform_data *pdata = info->pdata;
	struct device *dev = &pdata->pdev->dev;
	int err;
	int i;

	if (strncmp(input_dev->name, "gpio-keys", 9)) {
		return -ENODEV;
	}

	dev_info(dev, "connected to input device: %s", input_dev->name);

	info->input_dev = input_dev;
	info->handle.dev = input_dev;
	info->handle.handler = handler;
	info->handle.open = 0;
	info->handle.name = "p4wifi_jack_buttons";

	err = input_register_handle(&info->handle);
	if (err) {
		dev_err(dev, "failed to register buttons handle. error = %d\n",
			err);
		return err;
	}

	err = input_open_device(&info->handle);
	if (err) {
		dev_err(dev, "failed to open input device. error = %d\n", err);
		input_unregister_handle(&info->handle);
		return err;
	}

	for (i = 0; i < pdata->num_buttons_zones; i++)
		input_set_capability(input_dev, EV_KEY, pdata->buttons_zones[i].code);

	input_set_capability(input_dev, EV_SW, SW_MICROPHONE_INSERT);
	input_set_capability(input_dev, EV_SW, SW_HEADPHONE_INSERT);
	input_sync(info->input_dev);

	return 0;
}

static void p4wifi_jack_buttons_disconnect(struct input_handle *handle)
{
	struct jack_info *info = container_of(handle, struct jack_info,
		handle);
	struct jack_platform_data *pdata = info->pdata;
	struct device *dev = &pdata->pdev->dev;

	input_close_device(handle);
	input_unregister_handle(handle);

	info->input_dev = NULL;
	memset(&info->handle, 0, sizeof(struct input_handle));

	dev_dbg(dev, "disconnected input device.");
}

static void p4wifi_jack_set_type(struct jack_info *info, unsigned int jack_type)
{
	struct jack_platform_data *pdata = info->pdata;
	struct device *dev = &pdata->pdev->dev;
	int state;
	int enable_mic, inserted;

	if (!info->input_dev || jack_type == info->cur_jack_type) {
		return;
	}

	dev_info(dev, "jack type = %d\n", jack_type);
	info->cur_jack_type = jack_type;

	inserted = (jack_type & ~SEC_JACK_NO_DEVICE);
	enable_mic = (jack_type & SEC_HEADSET_4POLE);

	flush_work(&info->buttons_work);
	input_report_switch(info->input_dev, SW_HEADPHONE_INSERT, inserted);
	input_report_switch(info->input_dev, SW_MICROPHONE_INSERT,
		inserted && enable_mic);
	input_sync(info->input_dev);

	gpiod_set_value(pdata->ear_micbias_gpio, enable_mic);

	state = jack_type & (SEC_HEADSET_4POLE | SEC_HEADSET_3POLE) ? 1 : 0;
	extcon_set_state_sync(info->switch_jack_detection, EXTCON_JACK_HEADPHONE,
		state);
}

static unsigned int p4wifi_jack_determine_type(struct jack_info *info)
{
	struct jack_platform_data *pdata = info->pdata;
	struct device *dev = &pdata->pdev->dev;
	struct jack_detect_zone *zones = pdata->zones;
	int count[MAX_ZONE_LIMIT] = {0};
	int adc, gpio_value, i;
	unsigned int jack_type = SEC_JACK_NO_DEVICE;
	unsigned npolarity = !pdata->det_active_high;

	// Enable micbias gpio for adc read
	gpio_value = gpiod_get_value(pdata->ear_micbias_gpio);
	if (!gpio_value)
		gpiod_set_value(pdata->ear_micbias_gpio, 1);

	while (gpiod_get_value(pdata->detect_gpio) ^ npolarity) {
		adc = p4wifi_jack_read_adc(info);

		for (i = 0; i < pdata->num_zones; i++) {
			if (adc <= zones[i].adc_high) {
				if (++count[i] > zones[i].check_count) {
					jack_type = zones[i].jack_type;
					goto done;
				}

				if (zones[i].delay_ms > 0)
					msleep(zones[i].delay_ms);
				break;
			}
		}
	}

	dev_dbg(dev, "jack removed before detection complete\n");

done:
	return jack_type;
}

static irqreturn_t p4wifi_jack_detect_irq_thread(int irq, void *dev_id)
{
	struct jack_info *info = dev_id;
	struct jack_platform_data *pdata = info->pdata;
	unsigned int jack_type = SEC_JACK_NO_DEVICE;
	unsigned npolarity = !pdata->det_active_high;
	int time_left = DEBOUNCE_TIME;

	// Prevent suspend to allow user space to respond to switch
	//__pm_wakeup_event(&info->det_wakeup_source, WAKE_LOCK_TIME); FIXME

	// Debounce the interrupt
	while (time_left > 0) {
		unsigned int val = gpiod_get_value(pdata->detect_gpio) ^ npolarity;

		// Interrupt was deasserted. Skip the jack type detection.
		if (!val)
			goto done;

		msleep(10);
		time_left -= 10;
	}

	jack_type = p4wifi_jack_determine_type(info);

done:
	p4wifi_jack_set_type(info, jack_type);

	return IRQ_HANDLED;
}

void p4wifi_jack_buttons_work(struct work_struct *work)
{
	struct jack_info *info =
		container_of(work, struct jack_info, buttons_work);
	struct jack_platform_data *pdata = info->pdata;
	struct jack_button_zone *btn_zones = pdata->buttons_zones;
	struct device *dev = &pdata->pdev->dev;
	int adc, i;

	if ((info->cur_jack_type & SEC_HEADSET_4POLE) == 0) {
		dev_dbg(dev, "skip button detect work. cur_jack_type = 0x%X\n",
			info->cur_jack_type);
		return;
	}

	// Prevent suspend to allow user space to respond to switch
	//__pm_wakeup_event(&info->det_wakeup_source, WAKE_LOCK_TIME); FIXME

	if (info->pressed == 0) {
		input_report_key(info->input_dev, info->pressed_code, 0);
		input_sync(info->input_dev);
		dev_dbg(dev, "keycode %d is released\n", info->pressed_code);
		return;
	}

	adc = p4wifi_jack_read_adc(info);
	for (i = 0; i < pdata->num_buttons_zones; i++) {
		if (adc >= btn_zones[i].adc_low && adc <= btn_zones[i].adc_high) {

			info->pressed_code = btn_zones[i].code;
			input_report_key(info->input_dev, btn_zones[i].code, 1);
			input_sync(info->input_dev);

			dev_dbg(dev, "keycode %d is pressed\n", btn_zones[i].code);

			return;
		}
	}

	dev_warn(dev, "key was skipped. ADC value is %d\n", adc);
}

static int p4wifi_jack_read_adc(struct jack_info *info)
{
	struct device *dev = &info->pdata->pdev->dev;
	int val, err;

	err = iio_read_channel_raw(info->adc_channel, &val);
	if (err < 0) {
		dev_err(dev, "iio read channel failed. err = %d\n", err);
		val = 0;
	}

	dev_dbg(dev, "adc value = %d\n", val);
	return  val;
}

int p4wifi_jack_parse_dt(struct platform_device *pdev,
	struct jack_platform_data *pdata)
{
	struct device *dev = &pdev->dev;
	struct device_node *jack_zones_np, *button_zones_np;
	struct device_node *entry;
	struct device_node *np = pdev->dev.of_node;
	int i = 0;

	// jack zones
	jack_zones_np = of_get_child_by_name(np, "jack-zones");
	if (!jack_zones_np) {
		dev_err(dev, "could not find jack-zones node.\n");
		return -EINVAL;
	}

	pdata->num_zones = of_get_child_count(jack_zones_np);
	if (pdata->num_zones == 0) {
		pr_err("%s: no jack zones specified\n", of_node_full_name(np));
		return -EINVAL;
	}

	pdata->zones = devm_kzalloc(dev,
		sizeof(struct jack_detect_zone) * pdata->num_zones, GFP_KERNEL);

	for_each_child_of_node(jack_zones_np, entry) {
		struct jack_detect_zone *zone = &pdata->zones[i];
		u32 val;

		if (!of_property_read_u32(entry, "adc-high", &val))
			zone->adc_high = val;
		if (!of_property_read_u32(entry, "delay-ms", &val))
			zone->delay_ms = val;
		if (!of_property_read_u32(entry, "check-count", &val))
			zone->check_count = val;
		if (!of_property_read_u32(entry, "jack-type", &val))
			zone->jack_type = val;

		i++;
	}

	// button zones
	button_zones_np = of_get_child_by_name(np, "jack-button-zones");
	if (!button_zones_np) {
		dev_err(dev, "could not find jack-button-zones node\n");
		return -EINVAL;
	}

	pdata->num_buttons_zones = of_get_child_count(button_zones_np);
	if (pdata->num_buttons_zones == 0) {
		dev_err(dev, "no jack button zones specified\n");
	}

	pdata->buttons_zones = devm_kzalloc(dev,
		sizeof(struct jack_button_zone) * pdata->num_buttons_zones,
		GFP_KERNEL);

	i = 0;
	for_each_child_of_node(button_zones_np, entry) {
		struct jack_button_zone *button_zone = &pdata->buttons_zones[i];
		u32 val;

		if (!of_property_read_u32(entry, "code", &val))
			button_zone->code = val;
		if (!of_property_read_u32(entry, "adc-low", &val))
			button_zone->adc_low = val;
		if (!of_property_read_u32(entry, "adc-high", &val))
			button_zone->adc_high = val;

		i++;
	}

	return 0;
}

static int p4wifi_jack_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct jack_platform_data *pdata = NULL;
	struct jack_info *info = NULL;
	int err;

	pdata = devm_kzalloc(dev, sizeof(struct jack_platform_data),
		GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "No memory.\n");
		return -ENOMEM;
	}

	pdata->pdev = pdev;

	pdata->detect_gpio = devm_gpiod_get(dev, "jack-detect", GPIOD_ASIS);
	if (IS_ERR(pdata->detect_gpio))
		return PTR_ERR(pdata->detect_gpio);

	pdata->ear_micbias_gpio = devm_gpiod_get(dev, "ear-micbias", GPIOD_OUT_LOW);
	if (IS_ERR(pdata->ear_micbias_gpio))
		return PTR_ERR(pdata->ear_micbias_gpio);


	err = p4wifi_jack_parse_dt(pdev, pdata);
	if (err)
		return err;

	info = devm_kzalloc(dev, sizeof(struct jack_info), GFP_KERNEL);
	if (!info) {
		dev_err(dev, "No memory for jack info.\n");
		err = -ENOMEM;
		return err;
	}

	info->pdata = pdata;
	info->detect_irq = gpiod_to_irq(pdata->detect_gpio);

	info->adc_channel = devm_iio_channel_get(&pdev->dev,
		"headset-jack-detect");
	if (IS_ERR(info->adc_channel)) {
		err = PTR_ERR(info->adc_channel);
		if (err != -EPROBE_DEFER)
			dev_err(dev, "failed to get headset-jack-detect ADC channel\n");
		return err;
	}

	info->switch_jack_detection = devm_extcon_dev_allocate(dev, jack_cables);
	err = devm_extcon_dev_register(dev, info->switch_jack_detection);
	if (err < 0) {
		dev_err(dev, "failed to register extcon device\n");
		return err;
	}

	//wakeup_source_init(&info->det_wakeup_source, "headset-jack-detect"); FIXME

	INIT_WORK(&info->buttons_work, p4wifi_jack_buttons_work);
	info->queue = create_singlethread_workqueue("headset-jack-wq");
	if (info->queue == NULL) {
		err = -ENOMEM;
		dev_err(dev, "failed to create headset jack workqueue\n");
		goto err_create_wq_failed;
	}

	set_bit(EV_KEY, info->ids[0].evbit);
	info->ids[0].flags = INPUT_DEVICE_ID_MATCH_EVBIT;
	info->handler.filter = p4wifi_jack_buttons_filter;
	info->handler.connect = p4wifi_jack_buttons_connect;
	info->handler.disconnect = p4wifi_jack_buttons_disconnect;
	info->handler.name = "p4wifi_jack_buttons";
	info->handler.id_table = info->ids;
	info->handler.private = info;

	err = input_register_handler(&info->handler);
	if (err) {
		dev_err(dev, "failed to register input handler\n");
		goto err_register_input_handler;
	}

	err = devm_request_threaded_irq(dev, info->detect_irq, NULL,
				   p4wifi_jack_detect_irq_thread,
				   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   "headset-detect-interrupt", info);
	if (err) {
		dev_err(dev, "failed to request detect irq.\n");
		goto err_request_detect_irq;
	}

	err = enable_irq_wake(info->detect_irq);
	if (err) {
		dev_err(dev, "failed to enable_irq_wake.\n");
		goto err_regsister_data;
	}

	dev_set_drvdata(&pdev->dev, info);

	p4wifi_jack_determine_type(info);

	return 0;

err_regsister_data:
err_request_detect_irq:
	input_unregister_handler(&info->handler);
err_register_input_handler:
	destroy_workqueue(info->queue);
err_create_wq_failed:
	//wakeup_source_trash(&info->det_wakeup_source); FIXME

	return err;
}

static int p4wifi_jack_remove(struct platform_device *pdev)
{
	struct jack_info *info = dev_get_drvdata(&pdev->dev);

	disable_irq_wake(info->detect_irq);
	destroy_workqueue(info->queue);
	input_unregister_handler(&info->handler);
	//wakeup_source_trash(&info->det_wakeup_source); FIXME

	return 0;
}

static const struct of_device_id p4wifi_jack_of_ids[] = {
	{ .compatible = "samsung,p4wifi-headset-jack" },
	{ }
};

static struct platform_driver p4wifi_jack_driver = {
	.probe = p4wifi_jack_probe,
	.remove = p4wifi_jack_remove,
	.driver = {
		.name = "p4wifi-headset-jack",
		.of_match_table = p4wifi_jack_of_ids,
	},
};
module_platform_driver(p4wifi_jack_driver);

MODULE_AUTHOR("ms17.kim@samsung.com");
MODULE_DESCRIPTION("Samsung Electronics Corp Ear-Jack detection driver");
MODULE_LICENSE("GPL");
