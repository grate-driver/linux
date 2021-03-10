// SPDX-License-Identifier: GPL-2.0-only

#include <linux/gpio/machine.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#include <asm/setup.h>
#include <asm/system_info.h>

/* Boot Mode Physical Addresses and Magic Token */
#define BOOT_MODE_P_ADDR	(0x20000000 - 0x0C)
#define BOOT_MAGIC_P_ADDR	(0x20000000 - 0x10)
#define BOOT_MAGIC_TOKEN	0x626F6F74

/*
 * These defines must be kept in sync with the bootloader.
 */
enum {
	REBOOT_MODE_NONE,
	REBOOT_MODE_DOWNLOAD,
	REBOOT_MODE_NORMAL,
	REBOOT_MODE_UPDATE,
	REBOOT_MODE_RECOVERY,
	REBOOT_MODE_FOTA,
	REBOOT_MODE_FASTBOOT,
	REBOOT_MODE_DOWNLOAD_FAILED,
	REBOOT_MODE_DOWNLOAD_SUCCESS,
};

/*
 * We'll enumerate board revision from 10 to avoid a conflict with revision
 * numbers of P3.
 */
static const struct board_revision {
	unsigned int value;
	unsigned int gpio_value;
	const char *string;
} p4_board_rev[] = {
	{10, 0x16, "Rev00" },
	{11, 0x01, "Rev01" },
	{12, 0x02, "Rev02" },
	{13, 0x03, "Rev03" },
	{14, 0x04, "Rev04" },
};

struct p4wifi_data {
	struct notifier_block reboot_notifier;
	void __iomem *boot_magic_addr;
};

static struct gpiod_lookup_table p4wifi_gpio_lookup = {
	.dev_id = "p4wifi",
	.table = {
		GPIO_LOOKUP("tegra-gpio",  25, "v-accessory-5v", GPIO_ACTIVE_LOW),
		GPIO_LOOKUP("tegra-gpio",  70, "accessory-en", GPIO_LOOKUP_FLAGS_DEFAULT),
		GPIO_LOOKUP("tegra-gpio", 143, "otg-en", GPIO_LOOKUP_FLAGS_DEFAULT),
		GPIO_LOOKUP("tegra-gpio", 115, "cp-on", GPIO_LOOKUP_FLAGS_DEFAULT),
		GPIO_LOOKUP("tegra-gpio", 185, "cp-rst", GPIO_LOOKUP_FLAGS_DEFAULT),
		GPIO_LOOKUP("tegra-gpio",   9, "hw-rev0", GPIO_LOOKUP_FLAGS_DEFAULT),
		GPIO_LOOKUP("tegra-gpio",  87, "hw-rev1", GPIO_LOOKUP_FLAGS_DEFAULT),
		GPIO_LOOKUP("tegra-gpio", 164, "hw-rev2", GPIO_LOOKUP_FLAGS_DEFAULT),
		GPIO_LOOKUP("tegra-gpio",  48, "hw-rev3", GPIO_LOOKUP_FLAGS_DEFAULT),
		GPIO_LOOKUP("tegra-gpio",  49, "hw-rev4", GPIO_LOOKUP_FLAGS_DEFAULT),
		{ },
	},
};

/*
 * Set androidboot.mode=charger based on the Samsung p4 charging_mode parameter.
 */
static int __init charging_mode_arg(char *p)
{
	unsigned long charging_mode;

	if (kstrtoul(p, 16, &charging_mode))
		return 1;

	if (charging_mode)
		strlcat(boot_command_line, " androidboot.mode=charger",
			COMMAND_LINE_SIZE);

	return 0;
}
early_param("charging_mode", charging_mode_arg);

static int p4wifi_reboot(struct notifier_block *notifier,
			 unsigned long event, void *cmd)
{
	struct p4wifi_data *data = container_of(notifier, struct p4wifi_data,
						reboot_notifier);
	int mode;

	if (event == SYS_RESTART) {
		mode = REBOOT_MODE_NORMAL;
		if (cmd) {
			if (!strcmp(cmd, "recovery"))
				mode = REBOOT_MODE_RECOVERY;
			else if (!strcmp(cmd, "bootloader"))
				mode = REBOOT_MODE_FASTBOOT;
			else if (!strcmp(cmd, "fota"))
				mode = REBOOT_MODE_FOTA;
			else if (!strcmp(cmd, "download"))
				mode = REBOOT_MODE_DOWNLOAD;
		}
	} else {
		mode = REBOOT_MODE_NORMAL;
	}

	/*
	 * Write a magic value to a 2nd memory location to distinguish
	 * between a cold boot and a reboot.
	 */
	writel(BOOT_MAGIC_TOKEN, data->boot_magic_addr);

	pr_info("%s: mode %d\n", __func__, mode);

	return NOTIFY_DONE;
}

static int p4wifi_probe(struct platform_device *pdev)
{
	struct p4wifi_data *data;
	struct gpio_desc *gpiod;
	unsigned int i, value;
	void __iomem *io;

	/* disable unimplemented functionality */

	gpiod = devm_gpiod_get(&pdev->dev, "accessory-en", GPIOD_OUT_LOW);
	if (IS_ERR(gpiod))
		return dev_err_probe(&pdev->dev, PTR_ERR(gpiod),
				     "failed to get accessory-en GPIO\n");

	gpiod = devm_gpiod_get(&pdev->dev, "v-accessory-5v", GPIOD_IN);
	if (IS_ERR(gpiod)) {
		dev_err(&pdev->dev, "failed to get v-accessory-5v GPIO: %pe\n",
			gpiod);
		return PTR_ERR(gpiod);
	}

	dev_info(&pdev->dev, "accessory GPIO %s\n",
		 gpiod_get_value(gpiod) ? "enabled" : "disabled");

	gpiod = devm_gpiod_get(&pdev->dev, "otg-en", GPIOD_IN);
	if (IS_ERR(gpiod)) {
		dev_err(&pdev->dev, "failed to get otg-en GPIO: %pe\n",
			gpiod);
		return PTR_ERR(gpiod);
	}

	gpiod = devm_gpiod_get(&pdev->dev, "cp-on", GPIOD_OUT_LOW);
	if (IS_ERR(gpiod)) {
		dev_err(&pdev->dev, "failed to get cp-on GPIO: %pe\n",
			gpiod);
		return PTR_ERR(gpiod);
	}

	gpiod = devm_gpiod_get(&pdev->dev, "cp-rst", GPIOD_OUT_LOW);
	if (IS_ERR(gpiod)) {
		dev_err(&pdev->dev, "failed to get cp-rst GPIO: %pe\n",
			gpiod);
		return PTR_ERR(gpiod);
	}

	/* get revision */

	gpiod = devm_gpiod_get(&pdev->dev, "hw-rev0", GPIOD_IN);
	if (IS_ERR(gpiod)) {
		dev_err(&pdev->dev, "failed to get hw-rev0 GPIO: %pe\n",
			gpiod);
		return PTR_ERR(gpiod);
	}

	value = gpiod_get_value(gpiod);

	gpiod = devm_gpiod_get(&pdev->dev, "hw-rev1", GPIOD_IN);
	if (IS_ERR(gpiod)) {
		dev_err(&pdev->dev, "failed to get hw-rev1 GPIO: %pe\n",
			gpiod);
		return PTR_ERR(gpiod);
	}

	value |= gpiod_get_value(gpiod) << 1;

	gpiod = devm_gpiod_get(&pdev->dev, "hw-rev2", GPIOD_IN);
	if (IS_ERR(gpiod)) {
		dev_err(&pdev->dev, "failed to get hw-rev2 GPIO: %pe\n",
			gpiod);
		return PTR_ERR(gpiod);
	}

	value |= gpiod_get_value(gpiod) << 2;

	gpiod = devm_gpiod_get(&pdev->dev, "hw-rev3", GPIOD_IN);
	if (IS_ERR(gpiod)) {
		dev_err(&pdev->dev, "failed to get hw-rev3 GPIO: %pe\n",
			gpiod);
		return PTR_ERR(gpiod);
	}

	value |= gpiod_get_value(gpiod) << 3;

	gpiod = devm_gpiod_get(&pdev->dev, "hw-rev4", GPIOD_IN);
	if (IS_ERR(gpiod)) {
		dev_err(&pdev->dev, "failed to get hw-rev4 GPIO: %pe\n",
			gpiod);
		return PTR_ERR(gpiod);
	}

	value |= gpiod_get_value(gpiod) << 4;

	system_rev = p4_board_rev[ARRAY_SIZE(p4_board_rev) - 1].value;

	for (i = 0; i < ARRAY_SIZE(p4_board_rev); i++) {
		if (p4_board_rev[i].gpio_value == value) {
			system_rev = p4_board_rev[i].value;
			break;
		}
	}

	if (i == ARRAY_SIZE(p4_board_rev))
		dev_err(&pdev->dev, "valid revision NOT found\n");

	dev_info(&pdev->dev, "system_rev = %d (GPIO value = 0x%02x)\n",
		 system_rev, value);

	io = devm_ioremap(&pdev->dev, BOOT_MAGIC_P_ADDR, 4);
	if (!io) {
		dev_err(&pdev->dev, "ioremap failed\n");
		return -ENOMEM;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->reboot_notifier.notifier_call = p4wifi_reboot;
	data->reboot_notifier.priority = INT_MAX;
	data->boot_magic_addr = io;

	register_reboot_notifier(&data->reboot_notifier);

	return 0;
}

static struct platform_driver p4wifi_driver = {
	.probe = p4wifi_probe,
	.driver = {
		.name = "p4wifi",
	},
};
builtin_platform_driver(p4wifi_driver);

static int __init p4wifi(void)
{
	if (!of_machine_is_compatible("samsung,p4wifi"))
		return 0;

	gpiod_add_lookup_table(&p4wifi_gpio_lookup);
	platform_device_register_simple("p4wifi", -1, NULL, 0);

	return 0;
}
device_initcall(p4wifi);
