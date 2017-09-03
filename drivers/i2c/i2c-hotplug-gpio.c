// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * I2C hotplug gate controlled by GPIO
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
	struct device		*dev;
	struct gpio_desc	*gpio;
	int			 irq;
};

static inline struct i2c_adapter *i2c_hotplug_parent(struct i2c_adapter *adap)
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

static struct i2c_bus_recovery_info i2c_hotplug_recovery_info = {
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
	priv->adap.dev.parent = priv->dev;
	priv->adap.dev.of_node = priv->dev->of_node;

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
	msleep(20);

	if (gpiod_get_value_cansleep(priv->gpio))
		i2c_hotplug_activate(priv);
	else
		i2c_hotplug_deactivate(priv);

	return IRQ_HANDLED;
}

static void devm_i2c_put_adapter(void *adapter)
{
	i2c_put_adapter(adapter);
}

static int i2c_hotplug_gpio_probe(struct platform_device *pdev)
{
	struct i2c_adapter *parent;
	struct i2c_hotplug_priv *priv;
	bool is_i2c, is_smbus;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->dev = &pdev->dev;

	parent = of_get_i2c_adapter_by_phandle(&pdev->dev, "i2c-parent", 0);
	if (IS_ERR(parent))
		return dev_err_probe(&pdev->dev, PTR_ERR(parent),
				     "failed to get parent I2C adapter\n");

	priv->parent = parent;

	ret = devm_add_action_or_reset(&pdev->dev, devm_i2c_put_adapter,
				       parent);
	if (ret)
		return ret;

	priv->gpio = devm_gpiod_get(&pdev->dev, "detect", GPIOD_IN);
	if (IS_ERR(priv->gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->gpio),
				     "failed to get detect GPIO\n");

	is_i2c = parent->algo->master_xfer;
	is_smbus = parent->algo->smbus_xfer;

	snprintf(priv->adap.name, sizeof(priv->adap.name),
		 "i2c-hotplug (master i2c-%d)", i2c_adapter_id(parent));
	priv->adap.owner = THIS_MODULE;
	priv->adap.algo = i2c_hotplug_algo[is_i2c][is_smbus];
	priv->adap.algo_data = NULL;
	priv->adap.lock_ops = &i2c_hotplug_lock_ops;
	priv->adap.class = parent->class;
	priv->adap.retries = parent->retries;
	priv->adap.timeout = parent->timeout;
	priv->adap.quirks = parent->quirks;
	if (parent->bus_recovery_info)
		priv->adap.bus_recovery_info = &i2c_hotplug_recovery_info;

	if (!priv->adap.algo)
		return -EINVAL;

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return dev_err_probe(&pdev->dev, priv->irq,
				     "failed to get IRQ %d\n", priv->irq);

	ret = devm_request_threaded_irq(&pdev->dev, priv->irq, NULL,
					i2c_hotplug_interrupt,
					IRQF_ONESHOT | IRQF_SHARED,
					"i2c-hotplug", priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register IRQ %d\n", priv->irq);

	irq_wake_thread(priv->irq, priv);

	return 0;
}

static int i2c_hotplug_gpio_remove(struct platform_device *pdev)
{
	struct i2c_hotplug_priv *priv = platform_get_drvdata(pdev);

	i2c_hotplug_deactivate(priv);

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
