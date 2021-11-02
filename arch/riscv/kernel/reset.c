// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Regents of the University of California
 */

#include <linux/reboot.h>
#include <linux/pm.h>

static void default_power_off(void)
{
	while (1)
		wait_for_interrupt();
}

void machine_restart(char *cmd)
{
	do_kernel_restart(cmd);
	while (1);
}

void machine_halt(void)
{
	do_kernel_power_off();
	default_power_off();
}

void machine_power_off(void)
{
	do_kernel_power_off();
	default_power_off();
}
