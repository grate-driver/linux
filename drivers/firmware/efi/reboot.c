// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014 Intel Corporation; author Matt Fleming
 * Copyright (c) 2014 Red Hat, Inc., Mark Salter <msalter@redhat.com>
 */
#include <linux/efi.h>
#include <linux/reboot.h>

int efi_reboot_quirk_mode = -1;

void efi_reboot(enum reboot_mode reboot_mode, const char *__unused)
{
	const char *str[] = { "cold", "warm", "shutdown", "platform" };
	int efi_mode, cap_reset_mode;

	if (!efi_rt_services_supported(EFI_RT_SUPPORTED_RESET_SYSTEM))
		return;

	switch (reboot_mode) {
	case REBOOT_WARM:
	case REBOOT_SOFT:
		efi_mode = EFI_RESET_WARM;
		break;
	default:
		efi_mode = EFI_RESET_COLD;
		break;
	}

	/*
	 * If a quirk forced an EFI reset mode, always use that.
	 */
	if (efi_reboot_quirk_mode != -1)
		efi_mode = efi_reboot_quirk_mode;

	if (efi_capsule_pending(&cap_reset_mode)) {
		if (efi_mode != cap_reset_mode)
			printk(KERN_CRIT "efi: %s reset requested but pending "
			       "capsule update requires %s reset... Performing "
			       "%s reset.\n", str[efi_mode], str[cap_reset_mode],
			       str[cap_reset_mode]);
		efi_mode = cap_reset_mode;
	}

	efi.reset_system(efi_mode, EFI_SUCCESS, 0, NULL);
}

bool __weak efi_poweroff_required(void)
{
	return false;
}

static void efi_power_off(struct power_off_data *data)
{
	efi.reset_system(EFI_RESET_SHUTDOWN, EFI_SUCCESS, 0, NULL);
	/*
	 * The above call should not return, if it does fall back to
	 * the original power off method (typically ACPI poweroff).
	 */
}

static struct sys_off_handler efi_sys_off = {
	.power_off_priority = POWEROFF_PRIO_FIRMWARE + 1,
	.power_off_chaining_allowed = true,
	.power_off_cb = efi_power_off,
};

static int __init efi_shutdown_init(void)
{
	if (!efi_rt_services_supported(EFI_RT_SUPPORTED_RESET_SYSTEM))
		return -ENODEV;

	if (efi_poweroff_required())
		register_sys_off_handler(&efi_sys_off);

	return 0;
}
late_initcall(efi_shutdown_init);
