// SPDX-License-Identifier: GPL-2.0-only

#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/gpio.h>
#include <linux/gpio/machine.h>

#include <asm/setup.h>
#include <asm/system_info.h>

/******************************************************************************
* bootloader cmdline args
*****************************************************************************/
/*
 * Set androidboot.mode=charger based on the
 * Samsung p4 charging_mode parameter.
 */
static int __init charging_mode_arg(char *p)
{
	unsigned long charging_mode;

	if (kstrtoul(p, 16, &charging_mode))
		return 1;

	if (charging_mode)
		strlcat(boot_command_line, " androidboot.mode=charger", COMMAND_LINE_SIZE);

	return 0;
}
early_param("charging_mode", charging_mode_arg);

/******************************************************************************
* System revision
*****************************************************************************/

#define GPIO_HW_REV0		9  /* TEGRA_GPIO_PB1 */
#define GPIO_HW_REV1		87 /* TEGRA_GPIO_PK7 */
#define GPIO_HW_REV2		50 /* TEGRA_GPIO_PG2 */
#define GPIO_HW_REV3		48 /* TEGRA_GPIO_PG0 */
#define GPIO_HW_REV4		49 /* TEGRA_GPIO_PG1 */

struct board_revision {
	unsigned int value;
	unsigned int gpio_value;
	char string[20];
};

/* We'll enumerate board revision from 10
 * to avoid a confliction with revision numbers of P3
*/
static struct __init board_revision p4_board_rev[] = {
	{10, 0x16, "Rev00"},
	{11, 0x01, "Rev01"},
	{12, 0x02, "Rev02" },
	{13, 0x03, "Rev03" },
	{14, 0x04, "Rev04" },
};

static int __init p4wifi_init_hwrev(void)
{
	unsigned int value, rev_no, i;
	struct board_revision *board_rev;

	if (!of_machine_is_compatible("samsung,p4wifi"))
		return 0;

	pr_info("%s:\n", __func__);

	board_rev = p4_board_rev;
	rev_no = ARRAY_SIZE(p4_board_rev);

	gpio_request(GPIO_HW_REV0, "GPIO_HW_REV0");
	gpio_request(GPIO_HW_REV1, "GPIO_HW_REV1");
	gpio_request(GPIO_HW_REV2, "GPIO_HW_REV2");
	gpio_request(GPIO_HW_REV3, "GPIO_HW_REV3");
	gpio_request(GPIO_HW_REV4, "GPIO_HW_REV4");

	gpio_direction_input(GPIO_HW_REV0);
	gpio_direction_input(GPIO_HW_REV1);
	gpio_direction_input(GPIO_HW_REV2);
	gpio_direction_input(GPIO_HW_REV3);
	gpio_direction_input(GPIO_HW_REV4);

	value = gpio_get_value(GPIO_HW_REV0) |
			(gpio_get_value(GPIO_HW_REV1)<<1) |
			(gpio_get_value(GPIO_HW_REV2)<<2) |
			(gpio_get_value(GPIO_HW_REV3)<<3) |
			(gpio_get_value(GPIO_HW_REV4)<<4);

	gpio_free(GPIO_HW_REV0);
	gpio_free(GPIO_HW_REV1);
	gpio_free(GPIO_HW_REV2);
	gpio_free(GPIO_HW_REV3);
	gpio_free(GPIO_HW_REV4);

	for (i = 0; i < rev_no; i++) {
		if (board_rev[i].gpio_value == value)
			break;
	}

	system_rev = (i == rev_no) ? board_rev[rev_no-1].value : board_rev[i].value;

	if (i == rev_no)
		pr_warn("%s: Valid revision NOT found! Latest one will be assigned!\n", __func__);

	pr_info("%s: system_rev = %d (gpio value = 0x%02x)\n", __func__, system_rev, value);

	return 0;
}
arch_initcall(p4wifi_init_hwrev);

/* TODO Do following part of file using gpio-hogs. */

#define GPIO_ACCESSORY_EN	70 // TEGRA_GPIO_PI6
#define GPIO_V_ACCESSORY_5V	25  // TEGRA_GPIO_PD1
#define GPIO_OTG_EN		143 // TEGRA_GPIO_PR7
#define GPIO_CP_ON		115 //TEGRA_GPIO_PO3
#define GPIO_CP_RST		185 // TEGRA_GPIO_PX1

/*
 * Disable unimplemented functionality
 */
static int __init p4wifi_init_gpios(void)
{
	if (!of_machine_is_compatible("samsung,p4wifi"))
		return 0;

	pr_info("%s:\n", __func__);

	gpio_direction_output(GPIO_ACCESSORY_EN, 0);
	gpio_direction_input(GPIO_V_ACCESSORY_5V);
	pr_info("%s: accessory gpio %s\n", __func__,
		gpio_get_value(GPIO_V_ACCESSORY_5V)==0 ? "disabled" : "enabled");

	gpio_direction_output(GPIO_OTG_EN, 0);

	gpio_set_value(GPIO_CP_ON, 0);
	gpio_set_value(GPIO_CP_RST, 0);

	return 0;
}
postcore_initcall(p4wifi_init_gpios);
