// SPDX-License-Identifier: GPL-2.0-only

#include <linux/gpio.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/syscore_ops.h>
#include <linux/reboot.h>
#include <linux/file.h>
#include <linux/input.h>
#include <linux/of_platform.h>

#define DRV_NAME "p4wifi-reboot-mode"

#define GPIO_TA_nCONNECTED 178

/*
 * These defines must be kept in sync with the bootloader.
 */
#define REBOOT_MODE_NONE                0
#define REBOOT_MODE_DOWNLOAD            1
#define REBOOT_MODE_NORMAL              2
#define REBOOT_MODE_UPDATE              3
#define REBOOT_MODE_RECOVERY            4
#define REBOOT_MODE_FOTA                5
#define REBOOT_MODE_FASTBOOT            7
#define REBOOT_MODE_DOWNLOAD_FAILED     8
#define REBOOT_MODE_DOWNLOAD_SUCCESS    9


/* TODO FIXME This is very dangerous, especially that MMC aliases aren't
   set in the device-tree.
   If reboot works without touching MMC, then better not to touch it. */
#ifdef CONFIG_CMDLINE_PARTITION
#define MISC_DEVICE "/dev/mmcblk1p5"
#else
#define MISC_DEVICE "/dev/mmcblk1p6"
#endif


struct bootloader_message {
	char command[32];
	char status[32];
};

static int write_bootloader_message(char *cmd, int mode)
{
	struct file *filp;
	int ret = 0;
	loff_t pos = 2048L;  /* bootloader message offset in MISC.*/

	struct bootloader_message  bootmsg;

	memset(&bootmsg, 0, sizeof(struct bootloader_message));

	if (mode == REBOOT_MODE_RECOVERY) {
		strcpy(bootmsg.command, "boot-recovery");
	} else if (mode == REBOOT_MODE_FASTBOOT)
		strcpy(bootmsg.command, "boot-fastboot");
	else if (mode == REBOOT_MODE_NORMAL)
		strcpy(bootmsg.command, "boot-reboot");
	else if (mode == REBOOT_MODE_FOTA)
		strcpy(bootmsg.command, "boot-fota");
	else if (mode == REBOOT_MODE_NONE)
		strcpy(bootmsg.command, "boot-normal");
	else
		strcpy(bootmsg.command, cmd);

	bootmsg.status[0] = (char) mode;


	filp = filp_open(MISC_DEVICE, O_WRONLY | O_LARGEFILE, 0);

	if (IS_ERR(filp)) {
		pr_info("%s: failed to open MISC : '%s'.\n", DRV_NAME, MISC_DEVICE);
		return 0;
	}

	kernel_write(filp, (const char *)&bootmsg,
			sizeof(struct bootloader_message), &pos);

	vfs_fsync(filp, 0);
	filp_close(filp, NULL);

	if (ret < 0)
		pr_err("%s: failed to write on MISC\n", DRV_NAME);
	else
		pr_info("%s: command %s written on MISC\n",
			DRV_NAME, bootmsg.command);

	return ret;
}

/* Boot Mode Physical Addresses and Magic Token */
#define BOOT_MODE_P_ADDR	(0x20000000 - 0x0C)
#define BOOT_MAGIC_P_ADDR	(0x20000000 - 0x10)
#define BOOT_MAGIC_TOKEN	0x626F6F74

static void write_bootloader_mode(char boot_mode)
{
	void __iomem *to_io;
#if 0
	to_io = ioremap(BOOT_MODE_P_ADDR, 4);
	writel((unsigned long)boot_mode, to_io);
	iounmap(to_io);
#endif
	/*
	 * Write a magic value to a 2nd memory location to distinguish
	 * between a cold boot and a reboot.
	 */
	to_io = ioremap(BOOT_MAGIC_P_ADDR, 4);
	writel(BOOT_MAGIC_TOKEN, to_io);
	iounmap(to_io);
}

static int p4wifi_notifier_call(struct notifier_block *notifier,
				unsigned long event, void *cmd)
{
	int mode;
	u32 value;
	value = gpio_get_value(GPIO_TA_nCONNECTED);

	if (event == SYS_RESTART) {
		mode = REBOOT_MODE_NORMAL;
		if (cmd) {
			if (!strcmp((char *)cmd, "recovery"))
				mode = REBOOT_MODE_RECOVERY;
			else if (!strcmp((char *)cmd, "bootloader"))
				mode = REBOOT_MODE_FASTBOOT;
			else if (!strcmp((char *)cmd, "fota"))
				mode = REBOOT_MODE_FOTA;
			else if (!strcmp((char *)cmd, "download"))
				mode = REBOOT_MODE_DOWNLOAD;
		}
	} else if (event == SYS_POWER_OFF && !value)
		mode = REBOOT_MODE_NORMAL;
	else
		mode = REBOOT_MODE_NONE;

	write_bootloader_mode(mode);
	// FIXME we shouldn't write anything into hardcodeded partitions.
	// write_bootloader_message(cmd, mode);

	pr_info("%s: Reboot Mode %d\n", DRV_NAME, mode);

	return NOTIFY_DONE;
}

static struct notifier_block p4wifi_reboot_notifier = {
	.notifier_call = p4wifi_notifier_call,
	.priority = INT_MAX,
};

static int __init p4wifi_reboot_mode_init(void)
{
	if (!of_machine_is_compatible("samsung,p4wifi"))
		return 0;

	register_reboot_notifier(&p4wifi_reboot_notifier);
	pr_info("%s initialized\n", DRV_NAME);

	return 0;
}
module_init(p4wifi_reboot_mode_init);

static void __exit p4wifi_reboot_mode_exit(void)
{
	if (!of_machine_is_compatible("samsung,p4wifi"))
		return;

	unregister_reboot_notifier(&p4wifi_reboot_notifier);
}
module_exit(p4wifi_reboot_mode_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("p4wifi reboot mode");
