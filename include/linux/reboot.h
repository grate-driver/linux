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

/*
 * Standard restart priority levels. Intended to be set in the
 * sys_off_handler.restart_priority field.
 *
 * Use `RESTART_PRIO_ABC +- prio` style for additional levels.
 *
 * RESTART_PRIO_RESERVED:	Falls back to RESTART_PRIO_DEFAULT.
 *				Drivers may leave priority initialized
 *				to zero, to auto-set it to the default level.
 *
 * RESTART_PRIO_LOW:		Use this for handler of last resort.
 *
 * RESTART_PRIO_DEFAULT:	Use this for default/generic handler.
 *
 * RESTART_PRIO_HIGH:		Use this if you have multiple handlers and
 *				this handler has higher priority than the
 *				default handler.
 */
#define RESTART_PRIO_RESERVED		0
#define RESTART_PRIO_LOW		8
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


extern int register_reboot_notifier(struct notifier_block *);
extern int unregister_reboot_notifier(struct notifier_block *);

extern int devm_register_reboot_notifier(struct device *, struct notifier_block *);

extern void do_kernel_restart(char *cmd);

/*
 * System power-off and restart API.
 */

/*
 * Standard power-off priority levels. Intended to be set in the
 * sys_off_handler.power_off_priority field.
 *
 * Use `POWEROFF_PRIO_ABC +- prio` style for additional levels.
 *
 * POWEROFF_PRIO_RESERVED:	Falls back to POWEROFF_PRIO_DEFAULT.
 *				Drivers may leave priority initialized
 *				to zero, to auto-set it to the default level.
 *
 * POWEROFF_PRIO_PLATFORM:	Intended to be used by platform-level handler.
 *				Has lowest priority since device drivers are
 *				expected to take over platform handler which
 *				doesn't allow further callback chaining.
 *
 * POWEROFF_PRIO_DEFAULT:	Use this for default/generic handler.
 *
 * POWEROFF_PRIO_FIRMWARE:	Use this if handler uses firmware call.
 *				Has highest priority since firmware is expected
 *				to know best how to power-off hardware properly.
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

/**
 * struct power_off_data - Power-off callback argument
 *
 * @cb_data: Callback data.
 */
struct power_off_data {
	void *cb_data;
};

/**
 * struct power_off_prep_data - Power-off preparation callback argument
 *
 * @cb_data: Callback data.
 */
struct power_off_prep_data {
	void *cb_data;
};

/**
 * struct restart_data - Restart callback argument
 *
 * @cb_data: Callback data.
 * @cmd: Restart command string.
 * @stop_chain: Further lower priority callbacks won't be executed if set to
 *		true. Can be changed within callback. Default is false.
 * @mode: Reboot mode ID.
 */
struct restart_data {
	void *cb_data;
	const char *cmd;
	bool stop_chain;
	enum reboot_mode mode;
};

/**
 * struct reboot_prep_data - Reboot and shutdown preparation callback argument
 *
 * @cb_data: Callback data.
 * @cmd: Restart command string.
 * @stop_chain: Further lower priority callbacks won't be executed if set to
 *		true. Can be changed within callback. Default is false.
 * @mode: Preparation mode ID.
 */
struct reboot_prep_data {
	void *cb_data;
	const char *cmd;
	bool stop_chain;
	enum reboot_prepare_mode mode;
};

struct sys_off_handler_private_data {
	struct notifier_block power_off_nb;
	struct notifier_block restart_nb;
	struct notifier_block reboot_nb;
	void (*platform_power_off_cb)(void);
	void (*simple_power_off_cb)(void *data);
	void *simple_power_off_cb_data;
	bool registered;
};

/**
 * struct sys_off_handler - System power-off and restart handler
 *
 * @cb_data: Pointer to user's data.
 *
 * @power_off_cb: Callback that powers off this machine. Inactive if NULL.
 *
 * @power_off_prepare_cb: Power-off preparation callback. All power-off
 * preparation callbacks are invoked after @reboot_prepare_cb and before
 * @power_off_cb. Inactive if NULL.
 *
 * @power_off_priority: Power-off callback priority, must be unique.
 * Zero value is reserved and auto-reassigned to POWEROFF_PRIO_DEFAULT.
 * Inactive if @power_off_cb is NULL.
 *
 * @power_off_chaining_allowed: Set to false if callback's execution should
 * stop when @power_off_cb fails to power off this machine. True if further
 * lower priority power-off callback should be executed. False is default
 * value.
 *
 * @restart_cb: Callback that reboots this machine. Inactive if NULL.
 *
 * @restart_priority: Restart callback priority, must be unique. Zero value
 * is reserved and auto-reassigned to RESTART_PRIO_DEFAULT. Inactive if
 * @restart_cb is NULL.
 *
 * @restart_chaining_disallowed: Set to true if callback's execution should
 * stop when @restart_cb fails to restart this machine. False if further
 * lower priority restart callback should be executed. False is default
 * value.
 *
 * @reboot_prepare_cb: Reboot/shutdown preparation callback. All reboot
 * preparation callbacks are invoked before @restart_cb or @power_off_cb,
 * depending on the mode. It's registered with register_reboot_notifier().
 * The point is to remove boilerplate code from drivers which use this
 * callback in conjunction with the restart/power-off callbacks.
 *
 * @reboot_priority: Reboot/shutdown preparation callback priority, doesn't
 * need to be unique. Zero is default value. Inactive if @reboot_prepare_cb
 * is NULL.
 *
 * @priv: Internal data. Shouldn't be touched.
 *
 * Describes power-off and restart handlers which are invoked by kernel
 * to power off or restart this machine. Supports prioritized chaining for
 * both restart and power-off handlers.
 *
 * Struct sys_off_handler can be static. Members of this structure must not be
 * altered while handler is registered.
 *
 * Fill the structure members and pass it to @register_sys_off_handler().
 */
struct sys_off_handler {
	void *cb_data;

	void (*power_off_cb)(struct power_off_data *data);
	void (*power_off_prepare_cb)(struct power_off_prep_data *data);
	int power_off_priority;
	bool power_off_chaining_allowed;

	void (*restart_cb)(struct restart_data *data);
	int restart_priority;
	bool restart_chaining_disallowed;

	void (*reboot_prepare_cb)(struct reboot_prep_data *data);
	int reboot_priority;

	const struct sys_off_handler_private_data priv;
};

int register_sys_off_handler(struct sys_off_handler *handler);
int unregister_sys_off_handler(struct sys_off_handler *handler);

int devm_register_sys_off_handler(struct device *dev,
				  struct sys_off_handler *handler);

int devm_register_prioritized_power_off_handler(struct device *dev,
						int priority,
						void (*callback)(void *data),
						void *cb_data);

/**
 *	devm_register_simple_power_off_handler - Register simple power-off callback
 *	@dev: Device that registers callback
 *	@callback: Callback function
 *	@cb_data: Callback's argument
 *
 *	Registers resource-managed power-off callback with default priority.
 *	It will be invoked as last step of the power-off sequence. Further
 *	lower priority callbacks won't be executed if this @callback fails.
 *
 *	Returns zero on success, or error code on failure.
 */
static inline int
devm_register_simple_power_off_handler(struct device *dev,
				       void (*callback)(void *data),
				       void *cb_data)
{
	return devm_register_prioritized_power_off_handler(dev,
							   POWEROFF_PRIO_DEFAULT,
							   callback, cb_data);
}

int register_platform_power_off(void (*power_off)(void));
int unregister_platform_power_off(void (*power_off)(void));

int devm_register_prioritized_restart_handler(struct device *dev,
					      int priority,
					      void (*callback)(struct restart_data *data),
					      void *cb_data);

/**
 *	devm_register_simple_restart_handler - Register simple restart callback
 *	@dev: Device that registers callback
 *	@callback: Callback function
 *	@cb_data: Callback's argument
 *
 *	Registers resource-managed restart callback with default priority.
 *	It will be invoked as a part of the restart sequence. Further
 *	lower priority callback will be executed if this @callback fails.
 *
 *	Returns zero on success, or error code on failure.
 */
static inline int
devm_register_simple_restart_handler(struct device *dev,
				     void (*callback)(struct restart_data *data),
				     void *cb_data)
{
	return devm_register_prioritized_restart_handler(dev,
							 RESTART_PRIO_DEFAULT,
							 callback, cb_data);
}

void do_kernel_power_off(void);

/*
 * Architecture-specific implementations of sys_reboot commands.
 */

extern void migrate_to_reboot_cpu(void);
extern void machine_restart(char *cmd);
extern void machine_halt(void);
extern void machine_power_off(void);

extern void machine_shutdown(void);
struct pt_regs;
extern void machine_crash_shutdown(struct pt_regs *);

/*
 * Architecture independent implemenations of sys_reboot commands.
 */

extern void kernel_restart_prepare(char *cmd);
extern void kernel_restart(char *cmd);
extern void kernel_halt(void);
extern void kernel_power_off(void);
extern bool kernel_can_power_off(void);

extern int C_A_D; /* for sysctl */
void ctrl_alt_del(void);

#define POWEROFF_CMD_PATH_LEN	256
extern char poweroff_cmd[POWEROFF_CMD_PATH_LEN];

extern void orderly_poweroff(bool force);
extern void orderly_reboot(void);
void hw_protection_shutdown(const char *reason, int ms_until_forced);

/*
 * Emergency restart, callable from an interrupt handler.
 */

extern void emergency_restart(void);
#include <asm/emergency-restart.h>

#endif /* _LINUX_REBOOT_H */
