/*
 * drivers/motor/isa1200_vibrator.c
 *
 * Copyright (C) 2011 Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/slab.h>

#include <linux/mfd/core.h>
#include <linux/module.h>


#include <linux/hrtimer.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/pwm.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/workqueue.h>

#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/pinctrl/consumer.h>

#include <asm/mach-types.h>
#include "leds-isa1200.h"

#define AMPLITUDE_MIN     0
#define AMPLITUDE_MAX     254


struct isa1200_vibrator_drvdata {
	struct i2c_client *client;
	struct led_classdev cdev;

	struct clk *vib_clk;
	struct gpio_desc *enable_gpio;

	struct pinctrl* pinctrl;
	struct pinctrl_state *on_state;
	struct pinctrl_state *off_state;

	struct workqueue_struct *wq;
	struct delayed_work work;

	struct hrtimer timer;
	spinlock_t lock;
	int timeout;
	int max_timeout;

	bool running;

	u8 amplitude;

	u8 ctrl0;
	u8 ctrl1;
	u8 ctrl2;
	u8 ctrl4;
	u8 pll;
	u8 duty;
	u8 period;
};

/////////////////////////////////////////////////////////////////////////////////////

static int amplitude_to_duty(int period, int amplitude)
{
	int duty = (period * (amplitude + AMPLITUDE_MAX)) /
		(2 *(AMPLITUDE_MAX - AMPLITUDE_MIN));
	return duty;
}

static int isa1200_vibrator_i2c_write(struct i2c_client *client,
					u8 addr, u8 val)
{
	int error = 0;
	error = i2c_smbus_write_byte_data(client, addr, val);
	if (error)
		printk(KERN_ERR "[VIB] Failed to write addr=[0x%x], val=[0x%x]\n",
				addr, val);

	return error;
}

static void isa1200_vibrator_hw_init(struct isa1200_vibrator_drvdata *vib)
{
	msleep(20);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_CONTROL_REG0, vib->ctrl0);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_CONTROL_REG1, vib->ctrl1);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_CONTROL_REG2, vib->ctrl2);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_PLL_REG, vib->pll);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_CONTROL_REG4, vib->ctrl4);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_PWM_DUTY_REG, vib->period/2);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_PWM_PERIOD_REG, vib->period);

#ifdef MOTOR_DEBUG
	printk(KERN_DEBUG "[VIB] ctrl0 = 0x%x\n", vib->ctrl0);
	printk(KERN_DEBUG "[VIB] ctrl1 = 0x%x\n", vib->ctrl1);
	printk(KERN_DEBUG "[VIB] ctrl2 = 0x%x\n", vib->ctrl2);
	printk(KERN_DEBUG "[VIB] pll = 0x%x\n", vib->pll);
	printk(KERN_DEBUG "[VIB] ctrl4 = 0x%x\n", vib->ctrl4);
	printk(KERN_DEBUG "[VIB] duty = 0x%x\n", vib->period/2);
	printk(KERN_DEBUG "[VIB] period = 0x%x\n", vib->period);
#endif

}

static void isa1200_vibrator_on(struct isa1200_vibrator_drvdata *vib)
{
	int duty = vib->duty;

	pr_debug("%s\n", __func__);

	if (vib->duty >= vib->period) {
		duty -= 3;
	}

	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_CONTROL_REG0, vib->ctrl0 | CTL0_NORMAL_OP);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_PWM_DUTY_REG, vib->duty);
#ifdef MOTOR_DEBUG
	printk(KERN_DEBUG "[VIB] ctrl0 = 0x%x\n", vib->ctrl0 | CTL0_NORMAL_OP);
	printk(KERN_DEBUG "[VIB] duty = 0x%x\n", duty);
#endif
}

static void isa1200_vibrator_off(struct isa1200_vibrator_drvdata *vib)
{
	pr_debug("%s\n", __func__);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_PWM_DUTY_REG, vib->period/2);
	isa1200_vibrator_i2c_write(vib->client,
		HAPTIC_CONTROL_REG0, vib->ctrl0);
}

static void isa1200_vibrator_work(struct work_struct *work)
{
	struct isa1200_vibrator_drvdata *vib =
		container_of(to_delayed_work(work), struct isa1200_vibrator_drvdata, work);
	struct i2c_client* client = vib->client;
	int err;

	pr_debug("%s\n", __func__);

	if (vib->timeout == 0) {
		if (!vib->running)
			return;

		vib->running = false;
		isa1200_vibrator_off(vib);
		clk_disable_unprepare(vib->vib_clk);

		if (vib->pinctrl && vib->off_state) {
			err = pinctrl_select_state(vib->pinctrl, vib->off_state);
			if (err != 0)
				dev_err(&client->dev,
					"%s: error setting pinctrl off state. err=%d\n", __func__, err);
		}

	} else {
		if (vib->running)
			return;

		vib->running = true;

		if (vib->pinctrl && vib->on_state) {
			err = pinctrl_select_state(vib->pinctrl, vib->on_state);
			if (err != 0) {
				dev_err(&client->dev,
					"%s: error setting pinctrl on state. err=%d\n", __func__, err);
				return;
			}
		}

		clk_prepare_enable(vib->vib_clk);
		mdelay(1);
		isa1200_vibrator_on(vib);
	}
}

static enum hrtimer_restart isa1200_vibrator_timer_func(struct hrtimer *_timer)
{
	struct isa1200_vibrator_drvdata *vib =
		container_of(_timer, struct isa1200_vibrator_drvdata, timer);

	vib->timeout = 0;

	queue_delayed_work(vib->wq, &vib->work, 0);
	return HRTIMER_NORESTART;
}

/////////////////////////////////////////////////////////////////////////////////////

static void isa1200_brightness_set(struct led_classdev *led_cdev,
	enum led_brightness value)
{
	pr_info("%s: value=%d\n", __func__, value);

	led_cdev->brightness = value;
}

static int isa1200_blink_set(struct led_classdev *cdev,
	unsigned long *delay_on,
	unsigned long *delay_off)
{
	pr_info("%s\n", __func__);
	return 0;
}

static ssize_t enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct isa1200_vibrator_drvdata *vib =
			container_of(led_cdev, struct isa1200_vibrator_drvdata, cdev);
	unsigned long	flags;
	int value;

	sscanf(buf, "%d", &value);
	pr_debug("%s timeout=%d\n", __func__, value);

#ifdef MOTOR_DEBUG
	printk(KERN_DEBUG "[VIB] time = %dms\n", value);
#endif
	cancel_delayed_work(&vib->work);
	hrtimer_cancel(&vib->timer);
	vib->timeout = value;
	queue_delayed_work(vib->wq, &vib->work, 0);
	spin_lock_irqsave(&vib->lock, flags);
	if (value > 0) {
		if (value > vib->max_timeout)
			value = vib->max_timeout;

		hrtimer_start(&vib->timer,
			ns_to_ktime((u64)value * NSEC_PER_MSEC),
			HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&vib->lock, flags);

	return size;
}

static ssize_t amplitude_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct isa1200_vibrator_drvdata *vib =
			container_of(led_cdev, struct isa1200_vibrator_drvdata, cdev);

	return sprintf(buf, "%d\n", vib->amplitude);
}

static ssize_t amplitude_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct isa1200_vibrator_drvdata *vib =
			container_of(led_cdev, struct isa1200_vibrator_drvdata, cdev);
	int amplitude;

	sscanf(buf, "%d", &amplitude);

	if (amplitude > AMPLITUDE_MAX)
		amplitude = AMPLITUDE_MAX;
	else if (amplitude < AMPLITUDE_MIN)
		amplitude = AMPLITUDE_MIN;

	vib->duty = amplitude_to_duty(vib->period, amplitude);
	vib->amplitude = amplitude;

	pr_debug("%s: amplitude=%d duty_cycle=%d\n", __func__, amplitude, vib->duty);

	return size;
}


static struct device_attribute isa1200_device_attrs[] = {
	__ATTR(enable, S_IWUSR,
			NULL,
			enable_store),
	__ATTR(amplitude, S_IRUGO | S_IWUSR,
			amplitude_show,
			amplitude_store),
};

static int isa1200_init_pinctrl(struct isa1200_vibrator_drvdata *ddata)
{
	struct i2c_client *client = ddata->client;
	struct pinctrl *pinctrl;
	struct pinctrl_state *on_state, *off_state;
	int err = 0;

	pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR(pinctrl)) {
		dev_info(&client->dev,
			"%s: not using pinctrl.\n", __func__);
		return 0;
	}

	off_state = pinctrl_lookup_state(pinctrl, "off");
	if (IS_ERR(off_state)) {
		dev_err(&client->dev,
			"%s: error getting pinctrl off state\n", __func__);
		err = -ENODEV;
		goto err;
	}

	on_state = pinctrl_lookup_state(pinctrl, "on");
	if (IS_ERR(on_state)) {
		dev_err(&client->dev,
			"%s: error getting pinctrl on state\n", __func__);
		err = -ENODEV;
		goto err;
	}

	err = pinctrl_select_state(pinctrl, off_state);
	if (err) {
		dev_err(&client->dev,
			"%s: error setting pinctrl off state. err=%d\n", __func__, err);
		err = -ENODEV;
		goto err;
	}

	ddata->pinctrl = pinctrl;
	ddata->off_state = off_state;
	ddata->on_state = on_state;

	return 0;

err:
	devm_pinctrl_put(pinctrl);
	return err;
}

#ifdef CONFIG_OF
static int isa1200_parse_dt(struct i2c_client *client,
		struct isa1200_vibrator_drvdata *drvdata)
{
	struct device_node *np = client->dev.of_node;
	struct clk *vib_clk;
	int val, error;

	drvdata->enable_gpio = devm_gpiod_get_optional(&client->dev,
		"enable", GPIOD_OUT_HIGH);
	if (IS_ERR(drvdata->enable_gpio)) {
		error = PTR_ERR(drvdata->enable_gpio);
		dev_err(&client->dev, "Failed to get enable gpio: %d\n", error);
		return error;
	}

	if (!of_property_read_u32(np, "max-timeout", &val))
		drvdata->max_timeout = val;
	if (!of_property_read_u32(np, "ctrl0", &val))
		drvdata->ctrl0 = val;
	if (!of_property_read_u32(np, "ctrl1", &val))
		drvdata->ctrl1 = val;
	if (!of_property_read_u32(np, "ctrl2", &val))
		drvdata->ctrl2 = val;
	if (!of_property_read_u32(np, "ctrl4", &val))
		drvdata->ctrl4 = val;
	if (!of_property_read_u32(np, "pll", &val))
		drvdata->pll = val;
	if (!of_property_read_u32(np, "duty", &val))
		drvdata->duty = val;
	if (!of_property_read_u32(np, "period", &val))
		drvdata->period = val;

	vib_clk = of_clk_get_by_name(np, "vibrator-clk");
	if (vib_clk == NULL) {
		pr_err("%s: error getting clk.\n", __func__);
		return -ENODEV;
	}
	drvdata->vib_clk = vib_clk;

	return 0;
}
#else
static int isa1200_parse_dt(struct i2c_client *client,
		struct isa1200_vibrator_drvdata *drvdata)
{
	return -EINVAL;
}
#endif

static int isa1200_vibrator_i2c_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct isa1200_vibrator_platform_data *pdata = NULL;
	struct isa1200_vibrator_drvdata *ddata;
	int i, ret = 0;

	printk(KERN_DEBUG "[VIB] %s\n", __func__);

	ddata = kzalloc(sizeof(struct isa1200_vibrator_drvdata), GFP_KERNEL);
	if (NULL == ddata) {
		printk(KERN_ERR "[VIB] Failed to alloc memory\n");
		ret = -ENOMEM;
		goto err_free_mem;
	}

	if (client->dev.platform_data) {
		pdata = client->dev.platform_data;

		ddata->enable_gpio = pdata->enable_gpio;
		ddata->vib_clk = pdata->get_clk();
		ddata->ctrl0 = pdata->ctrl0;
		ddata->ctrl1 = pdata->ctrl1;
		ddata->ctrl2 = pdata->ctrl2;
		ddata->ctrl4 = pdata->ctrl4;
		ddata->pll = pdata->pll;
		ddata->duty = pdata->duty;
		ddata->period = pdata->period;
	} else if (client->dev.of_node) {
		ret = isa1200_parse_dt(client, ddata);
		if (ret) {
			pr_err("%s: error parsing device tree\n", __func__);
			goto err_free_mem;
		}
	}

	ddata->client = client;

	ddata->cdev.name = "isa1200";
	ddata->cdev.flags = LED_CORE_SUSPENDRESUME;
	ddata->cdev.brightness_set = isa1200_brightness_set;
	ddata->cdev.blink_set = isa1200_blink_set;
	ddata->cdev.default_trigger = "none";
	i2c_set_clientdata(client, ddata);

	ret = isa1200_init_pinctrl(ddata);
	if (ret)
		goto err_free_mem;

	isa1200_vibrator_hw_init(ddata);


	hrtimer_init(&ddata->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ddata->timer.function = isa1200_vibrator_timer_func;

	ddata->wq = create_singlethread_workqueue("isa1200");
	INIT_DELAYED_WORK(&ddata->work, isa1200_vibrator_work);

	ret = led_classdev_register(&client->dev, &ddata->cdev);
	if (ret < 0)
		goto err_free_mem;

	for (i = 0; i < ARRAY_SIZE(isa1200_device_attrs); i++) {
		ret = device_create_file(ddata->cdev.dev, &isa1200_device_attrs[i]);
		if (ret < 0) {
			dev_err(&client->dev,
					"%s: failed to create sysfs attributes\n", __func__);
			goto err_free_mem;
		}
	}

	return 0;

err_free_mem:
	kfree(ddata);
	return ret;

}

static int isa1200_vibrator_i2c_remove(struct i2c_client *client)
{
	struct isa1200_vibrator_drvdata *ddata  = i2c_get_clientdata(client);
	struct led_classdev *led_cdev = &ddata->cdev;
	int i;

	for (i = 0; i < ARRAY_SIZE(isa1200_device_attrs); i++) {
		 device_remove_file(led_cdev->dev, &isa1200_device_attrs[i]);
	}

	led_classdev_unregister(led_cdev);

	flush_workqueue(ddata->wq);
	destroy_workqueue(ddata->wq);
	ddata->wq = NULL;

	kfree(ddata);

	return 0;
}

#if 0
static int isa1200_vibrator_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct isa1200_vibrator_drvdata *vib  = i2c_get_clientdata(client);
	gpio_direction_output(vib->gpio_en, 0);
	return 0;
}

static int isa1200_vibrator_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct isa1200_vibrator_drvdata *vib  = i2c_get_clientdata(client);
	// isa1200_vibrator_hw_init(ddata);
	gpio_direction_output(vib->gpio_en, 1);
	return 0;
}

static SIMPLE_DEV_PM_OPS(isa1200_pm,
	isa1200_vibrator_suspend, isa1200_vibrator_resume);
#define ISA1200_PM &isa1200_pm
#else
#define ISA1200_PM NULL
#endif

static const struct i2c_device_id isa1200_vibrator_device_id[] = {
	{"isa1200_vibrator", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, isa1200_vibrator_device_id);

#ifdef CONFIG_OF
static const struct of_device_id isa1200_dt_match[] = {
	{ .compatible = "samsung_p3,isa1200_vibrator" },
	{ },
};
MODULE_DEVICE_TABLE(of, isa1200_dt_match);
#endif

static struct i2c_driver isa1200_vibrator_i2c_driver = {
	.driver = {
		.name = "isa1200_vibrator",
		.pm = ISA1200_PM,
		.of_match_table = of_match_ptr(isa1200_dt_match),
		.owner = THIS_MODULE,
	},
	.probe     = isa1200_vibrator_i2c_probe,
	.remove    = isa1200_vibrator_i2c_remove,
	.id_table  = isa1200_vibrator_device_id,
};

module_i2c_driver(isa1200_vibrator_i2c_driver);

MODULE_LICENSE("GPL");
