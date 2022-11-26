// SPDX-License-Identifier: GPL-2.0-or-later
/* 
 *    PDC early console support - use PDC firmware to dump text via boot console
 *
 *    Copyright (C) 2001-2022 Helge Deller <deller@gmx.de>
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/serial_core.h>
#include <asm/page.h>		/* for PAGE0 */
#include <asm/pdc.h>		/* for iodc_call() proto and friends */

static void pdc_console_write(struct console *co, const char *s, unsigned count)
{
	int i = 0;

	do {
		i += pdc_iodc_print(s + i, count - i);
	} while (i < count);
}

static int __init pdc_earlycon_setup(struct earlycon_device *device,
				     const char *opt)
{
	struct console *earlycon_console;

	/* If the console is duplex then copy the COUT parameters to CIN. */
	if (PAGE0->mem_cons.cl_class == CL_DUPLEX)
		memcpy(&PAGE0->mem_kbd, &PAGE0->mem_cons, sizeof(PAGE0->mem_cons));

	earlycon_console = device->con;
	earlycon_console->write = pdc_console_write;
	device->port.iotype = UPIO_MEM32BE;

	return 0;
}

EARLYCON_DECLARE(pdc, pdc_earlycon_setup);
