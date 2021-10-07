/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_REBOOT_H
#define _LINUX_REBOOT_H


#include <linux/notifier.h>
#include <uapi/linux/reboot.h>

struct device;

enum reboot_prepare_mode {
	SYS_DOWN = 1,		/* Notify of system down */
	SYS_RESTART = SYS_DOWN,
	SYS_HALT,		/* Notify of system halt */
	SYS_POWER_OFF,		/* Notify of system power off */
};

#define RESTART_PRIO_RESERVED		0
#define RESTART_PRIO_DEFAULT		128
#define RESTART_PRIO_HIGH		192

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
 * Unified poweroff + restart API.
 */

#define POWEROFF_PRIO_RESERVED		0
#define POWEROFF_PRIO_PLATFORM		1
#define POWEROFF_PRIO_DEFAULT		128
#define POWEROFF_PRIO_HIGH		192
#define POWEROFF_PRIO_FIRMWARE		224

enum poweroff_mode {
	POWEROFF_NORMAL = 0,
	POWEROFF_PREPARE,
};

struct power_off_data {
	void *cb_data;
};

struct power_off_prep_data {
	void *cb_data;
};

struct restart_data {
	void *cb_data;
	const char *cmd;
	enum reboot_mode mode;
};

struct reboot_prep_data {
	void *cb_data;
	const char *cmd;
	enum reboot_prepare_mode mode;
};

struct power_handler_private_data {
	struct notifier_block reboot_prep_nb;
	struct notifier_block power_off_nb;
	struct notifier_block restart_nb;
	void (*trivial_power_off_cb)(void);
	void (*simple_power_off_cb)(void *data);
	void *simple_power_off_cb_data;
	bool registered;
};

/**
 * struct power_handler - Power-off + restart handlers
 *
 * Describes power-off and restart handlers which are invoked by kernel
 * to power off or restart this machine. Struct power_handler can be static.
 * Members of this structure must not be altered while handler is registered.
 * Fill the structure members and pass it to register_power_handler().
 */
struct power_handler {
	/**
	 * @cb_data:
	 *
	 * User data included in callback's argument.
	 */
	void *cb_data;

	/**
	 * @power_off_cb:
	 *
	 * Callback that should turn off machine. Inactive if NULL.
	 */
	void (*power_off_cb)(struct power_off_data *data);

	/**
	 * @power_off_prepare_cb:
	 *
	 * Power-off preparation callback. All power-off preparation callbacks
	 * are invoked before @restart_cb. Inactive if NULL.
	 */
	void (*power_off_prepare_cb)(struct power_off_prep_data *data);

	/**
	 * @power_off_priority:
	 *
	 * Power-off callback priority, must be unique. Zero value is reassigned
	 * to default priority. Inactive if @power_off_cb is NULL.
	 */
	int power_off_priority;

	/**
	 * @power_off_chaining_allowed:
	 *
	 * False if callbacks execution should stop when @power_off_cb fails
	 * to power off machine. True if further lower priority  power-off
	 * callback should be executed.
	 */
	bool power_off_chaining_allowed;

	/**
	 * @restart_cb:
	 *
	 * Callback that should reboot machine. Inactive if NULL.
	 */
	void (*restart_cb)(struct restart_data *data);

	/**
	 * @restart_priority:
	 *
	 * Restart callback priority, must be unique. Zero value is reassigned
	 * to default priority. Inactive if @restart_cb is NULL.
	 */
	int restart_priority;

	/**
	 * @reboot_prepare_cb:
	 *
	 * Reboot preparation callback. All reboot preparation callbacks are
	 * invoked before @restart_cb. Inactive if NULL.
	 */
	void (*reboot_prepare_cb)(struct reboot_prep_data *data);

	/**
	 * @priv:
	 *
	 * Internal data. Shouldn't be touched.
	 */
	const struct power_handler_private_data priv;
};

int register_power_handler(struct power_handler *handler);
void unregister_power_handler(struct power_handler *handler);

struct power_handler *
register_simple_power_off_handler(void (*callback)(void *data), void *data);

void unregister_simple_power_off_handler(struct power_handler *handler);

int devm_register_power_handler(struct device *dev,
				struct power_handler *handler);

int devm_register_simple_power_off_handler(struct device *dev,
					   void (*callback)(void *data),
					   void *data);

int devm_register_trivial_power_off_handler(struct device *dev,
					    void (*callback)(void));

int devm_register_simple_restart_handler(struct device *dev,
					 void (*callback)(struct restart_data *data),
					 void *data);

int devm_register_prioritized_restart_handler(struct device *dev,
					      int priority,
					      void (*callback)(struct restart_data *data),
					      void *data);

int register_platform_power_off(void (*power_off)(void));

void do_kernel_power_off(void);

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
bool kernel_can_power_off(void);

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
