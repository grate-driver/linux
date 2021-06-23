// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASUS EC driver - charging monitoring
 *
 * Written by: Svyatoslav Ryhel <clamor95@gmail.com>
 *
 * Copyright (C) 2021 Svyatoslav Ryhel
 *
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/mfd/asus-ec.h>

#define ASUSEC_CHARGER_DELAY_MSEC		1000
#define ASUSEC_CHARGER_AC_MASK			0x20

/*
 * Embedded controller gives reaction on plug events from reg 0x0A 
 * like in asusec_charger_callback function. This table represents all
 * EC reactions on different 40-pin connector events. Use wise.
 *
 * PAD-ec no-plug  0x42 / PAD-ec DOCK     0x22 / DOCK-ec no-plug 0x42
 * PAD-ec AC       0x27 / PAD-ec DOCK+AC  0x26 / DOCK-ec AC      0x27
 * PAD-ec USB      0x47 / PAD-ec DOCK+USB 0x26 / DOCK-ec USB     0x43
 *
 */

struct asusec_charger_data {
	const struct asusec_info		*ec;
	struct power_supply			*charger;
	struct delayed_work			poll_work;
	struct mutex				charger_lock;
	unsigned long				charger_data_ts;
	int					last_state;
	int					charger_addr;
	u8					charger_data[DOCKRAM_ENTRY_BUFSIZE];
};

static enum power_supply_property asusec_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int asusec_charger_callback(struct asusec_charger_data *priv)
{
	int ret = 0;

	msleep(ASUSEC_CHARGER_DELAY_MSEC);

	mutex_lock(&priv->charger_lock);

	if (time_before(jiffies, priv->charger_data_ts)) {
		mutex_unlock(&priv->charger_lock);
		return priv->last_state;
	}

	ret = asus_dockram_read(priv->ec->dockram, priv->charger_addr, priv->charger_data);
	if (ret < 0) {
		mutex_unlock(&priv->charger_lock);
		return -EREMOTEIO;
	}

	priv->charger_data_ts = jiffies + msecs_to_jiffies(ASUSEC_CHARGER_DELAY_MSEC);

	mutex_unlock(&priv->charger_lock);

	ret = priv->charger_data[1] & ASUSEC_CHARGER_AC_MASK;
	if (ret)
		return 1;

	return 0;
}

static int asusec_charger_get_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct asusec_charger_data *priv = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = asusec_charger_callback(priv);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static void asusec_charger_poll_work(struct work_struct *work)
{
	struct asusec_charger_data *priv =
		container_of(work, struct asusec_charger_data, poll_work.work);
	int state = asusec_charger_callback(priv);

	if (state != priv->last_state) {
		priv->last_state = state;
		power_supply_changed(priv->charger);
	}

	schedule_delayed_work(&priv->poll_work,
			      msecs_to_jiffies(ASUSEC_CHARGER_DELAY_MSEC));
}

static const struct power_supply_desc pad_charger_desc = {
	.name = "pad-ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = asusec_charger_properties,
	.num_properties = ARRAY_SIZE(asusec_charger_properties),
	.get_property = asusec_charger_get_property,
};

static const struct power_supply_desc dock_charger_desc = {
	.name = "dock-ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = asusec_charger_properties,
	.num_properties = ARRAY_SIZE(asusec_charger_properties),
	.get_property = asusec_charger_get_property,
};

static int asusec_charger_probe(struct platform_device *pdev)
{
	const struct asusec_info *ec = asusec_cell_to_ec(pdev);
	const struct power_supply_desc *psd;
	struct asusec_charger_data *priv;
	struct asusec_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct power_supply_config cfg = {};

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	mutex_init(&priv->charger_lock);

	priv->ec = ec;
	priv->charger_addr = pdata->charger_addr;
	priv->charger_data_ts = jiffies - 1;
	priv->last_state = asusec_charger_callback(priv);

	cfg.of_node = pdev->dev.parent->of_node;
	cfg.drv_data = priv;

	if (of_device_is_compatible(cfg.of_node, "asus,pad-ec"))
		psd = &pad_charger_desc;
	else
		psd = &dock_charger_desc;

	priv->charger = devm_power_supply_register(&pdev->dev, psd, &cfg);
	if (IS_ERR(priv->charger))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->charger),
				     "Failed to register power supply\n");

	INIT_DELAYED_WORK(&priv->poll_work, asusec_charger_poll_work);
	schedule_delayed_work(&priv->poll_work,
			      msecs_to_jiffies(ASUSEC_CHARGER_DELAY_MSEC));

	return 0;
}

static int asusec_charger_remove(struct platform_device *pdev)
{
	struct asusec_charger_data *priv = dev_get_drvdata(&pdev->dev);

	cancel_delayed_work_sync(&priv->poll_work);

	return 0;
}

static int __maybe_unused asusec_charger_suspend(struct device *dev)
{
	struct asusec_charger_data *priv = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&priv->poll_work);

	return 0;
}

static int __maybe_unused asusec_charger_resume(struct device *dev)
{
	struct asusec_charger_data *priv = dev_get_drvdata(dev);

	schedule_delayed_work(&priv->poll_work,
			      msecs_to_jiffies(ASUSEC_CHARGER_DELAY_MSEC));

	return 0;
}

static SIMPLE_DEV_PM_OPS(asusec_charger_pm_ops,
			 asusec_charger_suspend, asusec_charger_resume);

static struct platform_driver asusec_charger_driver = {
	.driver = {
		.name = "asusec-charger",
		.pm = &asusec_charger_pm_ops,
	},
	.probe = asusec_charger_probe,
	.remove = asusec_charger_remove,
};
module_platform_driver(asusec_charger_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("ASUS Transformer Pad charger driver");
MODULE_LICENSE("GPL");
