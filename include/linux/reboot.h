/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_REBOOT_H
#define _LINUX_REBOOT_H


#include <linux/notifier.h>
#include <uapi/linux/reboot.h>

struct device;

#define SYS_DOWN	0x0001	/* Notify of system down */
#define SYS_RESTART	SYS_DOWN
#define SYS_HALT	0x0002	/* Notify of system halt */
#define SYS_POWER_OFF	0x0003	/* Notify of system power off */

enum reboot_mode {
	REBOOT_UNDEFINED = -1,
	REBOOT_COLD = 0,
	REBOOT_WARM,
	REBOOT_HARD,
	REBOOT_SOFT,
	REBOOT_GPIO,
};
extern enum reboot_mode reboot_mode;
extern enum reboot_mode panic_reboot_mode;

enum reboot_type {
	BOOT_TRIPLE	= 't',
	BOOT_KBD	= 'k',
	BOOT_BIOS	= 'b',
	BOOT_ACPI	= 'a',
	BOOT_EFI	= 'e',
	BOOT_CF9_FORCE	= 'p',
	BOOT_CF9_SAFE	= 'q',
};
extern enum reboot_type reboot_type;

extern int reboot_default;
extern int reboot_cpu;
extern int reboot_force;


int register_reboot_notifier(struct notifier_block *);
int unregister_reboot_notifier(struct notifier_block *);

int devm_register_reboot_notifier(struct device *, struct notifier_block *);

int register_restart_handler(struct notifier_block *);
int unregister_restart_handler(struct notifier_block *);
void do_kernel_restart(char *cmd);

/*
 * Architecture-specific implementations of sys_reboot commands.
 */

void migrate_to_reboot_cpu(void);
void machine_restart(char *cmd);
void machine_halt(void);
void machine_power_off(void);

void machine_shutdown(void);
struct pt_regs;
void machine_crash_shutdown(struct pt_regs *);

/*
 * Architecture independent implementations of sys_reboot commands.
 */

void kernel_restart_prepare(char *cmd);
void kernel_restart(char *cmd);
void kernel_halt(void);
void kernel_power_off(void);

extern int C_A_D; /* for sysctl */
void ctrl_alt_del(void);

#define POWEROFF_CMD_PATH_LEN	256
extern char poweroff_cmd[POWEROFF_CMD_PATH_LEN];

void orderly_poweroff(bool force);
void orderly_reboot(void);
void hw_protection_shutdown(const char *reason, int ms_until_forced);

/*
 * Emergency restart, callable from an interrupt handler.
 */

void emergency_restart(void);
#include <asm/emergency-restart.h>

#endif /* _LINUX_REBOOT_H */
