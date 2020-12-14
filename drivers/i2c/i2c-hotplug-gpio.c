/*
 * I2C multiplexer using GPIO API
 *
 * Peter Korsgaard <peter.korsgaard@barco.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct i2c_hotplug_priv {
	struct i2c_adapter	 adap;
	struct i2c_adapter	*parent;
	struct device		*adap_dev;
	struct gpio_desc	*gpio;
	int			 irq;
};

static struct i2c_adapter *i2c_hotplug_parent(struct i2c_adapter *adap)
{
	struct i2c_hotplug_priv *priv = container_of(adap, struct i2c_hotplug_priv, adap);

	return priv->parent;
}

static int i2c_hotplug_master_xfer(struct i2c_adapter *adap,
				   struct i2c_msg msgs[], int num)
{
	struct i2c_adapter *parent = i2c_hotplug_parent(adap);

	return parent->algo->master_xfer(parent, msgs, num);
}

static int i2c_hotplug_smbus_xfer(struct i2c_adapter *adap, u16 addr,
				  unsigned short flags, char read_write,
				  u8 command, int protocol,
				  union i2c_smbus_data *data)
{
	struct i2c_adapter *parent = i2c_hotplug_parent(adap);

	return parent->algo->smbus_xfer(parent, addr, flags, read_write,
					command, protocol, data);
}

static u32 i2c_hotplug_functionality(struct i2c_adapter *adap)
{
	u32 parent_func = i2c_get_functionality(i2c_hotplug_parent(adap));

	return parent_func & ~I2C_FUNC_SLAVE;
}

static const struct i2c_algorithm i2c_hotplug_algo_i2c = {
	.master_xfer = i2c_hotplug_master_xfer,
	.functionality = i2c_hotplug_functionality,
};

static const struct i2c_algorithm i2c_hotplug_algo_smbus = {
	.smbus_xfer = i2c_hotplug_smbus_xfer,
	.functionality = i2c_hotplug_functionality,
};

static const struct i2c_algorithm i2c_hotplug_algo_both = {
	.master_xfer = i2c_hotplug_master_xfer,
	.smbus_xfer = i2c_hotplug_smbus_xfer,
	.functionality = i2c_hotplug_functionality,
};

static const struct i2c_algorithm *const i2c_hotplug_algo[2][2] = {
	/* non-I2C */
	{ NULL, &i2c_hotplug_algo_smbus },
	/* I2C */
	{ &i2c_hotplug_algo_i2c, &i2c_hotplug_algo_both }
};

static void i2c_hotplug_lock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	i2c_lock_bus(i2c_hotplug_parent(adap), flags);
}

static int i2c_hotplug_trylock_bus(struct i2c_adapter *adap,
				   unsigned int flags)
{
	return i2c_trylock_bus(i2c_hotplug_parent(adap), flags);
}

static void i2c_hotplug_unlock_bus(struct i2c_adapter *adap,
				   unsigned int flags)
{
	i2c_unlock_bus(i2c_hotplug_parent(adap), flags);
}

static const struct i2c_lock_operations i2c_hotplug_lock_ops = {
	.lock_bus =    i2c_hotplug_lock_bus,
	.trylock_bus = i2c_hotplug_trylock_bus,
	.unlock_bus =  i2c_hotplug_unlock_bus,
};

static int i2c_hotplug_recover_bus(struct i2c_adapter *adap)
{
	return i2c_recover_bus(i2c_hotplug_parent(adap));
}

static const struct i2c_bus_recovery_info i2c_hotplug_recovery_info = {
	.recover_bus = i2c_hotplug_recover_bus,
};

static int i2c_hotplug_activate(struct i2c_hotplug_priv *priv)
{
	int ret;

	if (priv->adap.algo_data)
		return 0;

	/*
	 * Store the dev data in adapter dev, since
	 * previous i2c_del_adapter might have wiped it.
	 */
	priv->adap.dev.parent = priv->adap_dev;
	priv->adap.dev.of_node = priv->adap_dev->of_node;

	dev_dbg(priv->adap.dev.parent, "connection detected");

	ret = i2c_add_adapter(&priv->adap);
	if (!ret)
		priv->adap.algo_data = (void *)1;

	return ret;
}

static void i2c_hotplug_deactivate(struct i2c_hotplug_priv *priv)
{
	if (!priv->adap.algo_data)
		return;

	dev_dbg(priv->adap.dev.parent, "disconnection detected");

	i2c_del_adapter(&priv->adap);
	priv->adap.algo_data = NULL;
}

static irqreturn_t i2c_hotplug_interrupt(int irq, void *dev_id)
{
	struct i2c_hotplug_priv *priv = dev_id;

	/* debounce */
	msleep(10);

	if (gpiod_get_value_cansleep(priv->gpio))
		i2c_hotplug_activate(priv);
	else
		i2c_hotplug_deactivate(priv);

	return IRQ_HANDLED;
}

static int i2c_hotplug_gpio_probe(struct platform_device *pdev)
{
	struct device_node *parent_np;
	struct i2c_hotplug_priv *priv;
	struct i2c_adapter *parent;
	bool is_i2c, is_smbus;
	int err;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	platform_set_drvdata(pdev, priv);

	parent_np = of_parse_phandle(pdev->dev.of_node, "i2c-parent", 0);
	if (!parent_np) {
		dev_err(&pdev->dev, "Cannot parse i2c-parent\n");
		return -ENODEV;
	}

	priv->parent = parent = of_find_i2c_adapter_by_node(parent_np);
	of_node_put(parent_np);
	if (!parent)
		return -ENODEV;

	priv->gpio = devm_gpiod_get(&pdev->dev, "detect", GPIOD_IN);
	if (IS_ERR(priv->gpio)) {
		err = PTR_ERR(priv->gpio);
		if (err != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Cannot get detect-gpio: %d\n",
				err);
		goto err_parent;
	}

	is_i2c = parent->algo->master_xfer;
	is_smbus = parent->algo->smbus_xfer;

	snprintf(priv->adap.name, sizeof(priv->adap.name),
		 "i2c-hotplug (master i2c-%d)", i2c_adapter_id(parent));
	priv->adap.owner = THIS_MODULE;
	priv->adap.algo = i2c_hotplug_algo[is_i2c][is_smbus];
	priv->adap.algo_data = NULL;
	priv->adap.lock_ops = &i2c_hotplug_lock_ops;
	priv->adap_dev = &pdev->dev;
	priv->adap.class = parent->class;
	priv->adap.retries = parent->retries;
	priv->adap.timeout = parent->timeout;
	priv->adap.quirks = parent->quirks;
	if (parent->bus_recovery_info)
		/* .bus_recovery_info is not const, but won't be modified */
		priv->adap.bus_recovery_info = (void *)&i2c_hotplug_recovery_info;

	err = -EINVAL;
	if (!priv->adap.algo)
		goto err_parent;

	priv->irq = err = platform_get_irq(pdev, 0);
	if (err < 0) {
		if (err != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Cannot find IRQ: %d\n", err);
		goto err_parent;
	}

	err = request_threaded_irq(priv->irq, NULL, i2c_hotplug_interrupt,
				   IRQF_ONESHOT | IRQF_SHARED,
				   "i2c-hotplug", priv);
	if (err)
		goto err_parent;

	irq_wake_thread(priv->irq, priv);

	return 0;

err_parent:
	i2c_put_adapter(parent);
	return err;
}

static int i2c_hotplug_gpio_remove(struct platform_device *pdev)
{
	struct i2c_hotplug_priv *priv = platform_get_drvdata(pdev);

	free_irq(priv->irq, priv);
	i2c_hotplug_deactivate(priv);
	i2c_put_adapter(priv->parent);

	return 0;
}

static const struct of_device_id i2c_hotplug_gpio_of_match[] = {
	{ .compatible = "i2c-hotplug-gpio" },
	{},
};
MODULE_DEVICE_TABLE(of, i2c_hotplug_gpio_of_match);

static struct platform_driver i2c_hotplug_gpio_driver = {
	.probe	= i2c_hotplug_gpio_probe,
	.remove	= i2c_hotplug_gpio_remove,
	.driver	= {
		.name	= "i2c-hotplug-gpio",
		.of_match_table = i2c_hotplug_gpio_of_match,
	},
};

module_platform_driver(i2c_hotplug_gpio_driver);

MODULE_DESCRIPTION("Hot-plugged I2C bus detected by GPIO");
MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_LICENSE("GPL");
