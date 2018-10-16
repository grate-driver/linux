/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2009-2013, NVIDIA Corporation. All rights reserved.
 */

#ifndef __LINUX_HOST1X_H
#define __LINUX_HOST1X_H

#include <linux/device.h>
#include <linux/types.h>

enum host1x_class {
	HOST1X_CLASS_HOST1X = 0x1,
	HOST1X_CLASS_GR2D = 0x51,
	HOST1X_CLASS_GR2D_SB = 0x52,
	HOST1X_CLASS_VIC = 0x5D,
	HOST1X_CLASS_GR3D = 0x60,
};

struct host1x;
struct host1x_client;

u64 host1x_get_dma_mask(struct host1x *host1x);

/**
 * struct host1x_client_ops - host1x client operations
 * @init: host1x client initialization code
 * @exit: host1x client tear down code
 * @suspend: host1x client suspend code
 * @resume: host1x client resume code
 */
struct host1x_client_ops {
	int (*init)(struct host1x_client *client);
	int (*exit)(struct host1x_client *client);
	int (*suspend)(struct host1x_client *client);
	int (*resume)(struct host1x_client *client);
};

/**
 * struct host1x_client - host1x client structure
 * @list: list node for the host1x client
 * @host: pointer to struct device representing the host1x controller
 * @dev: pointer to struct device backing this host1x client
 * @ops: host1x client operations
 * @syncpts: array of syncpoints requested for this client
 * @num_syncpts: number of syncpoints requested for this client
 */
struct host1x_client {
	struct list_head list;
	struct device *host;
	struct device *dev;
	const struct host1x_client_ops *ops;
	struct host1x_syncpt **syncpts;
	unsigned int num_syncpts;

	struct host1x_client *parent;
	unsigned int usecount;
	struct mutex lock;
};

/*
 * host1x syncpoints
 */

#define HOST1X_SYNCPT_CLIENT_MANAGED	(1 << 0)
#define HOST1X_SYNCPT_HAS_BASE		(1 << 1)

struct host1x_syncpt_base;
struct host1x_syncpt;
struct host1x;

struct host1x_syncpt *host1x_syncpt_get(struct host1x *host, u32 id);
u32 host1x_syncpt_id(struct host1x_syncpt *sp);
u32 host1x_syncpt_read_min(struct host1x_syncpt *sp);
u32 host1x_syncpt_read_max(struct host1x_syncpt *sp);
u32 host1x_syncpt_read(struct host1x_syncpt *sp);
int host1x_syncpt_incr(struct host1x_syncpt *sp);
u32 host1x_syncpt_incr_max(struct host1x_syncpt *sp, u32 incrs);
int host1x_syncpt_wait(struct host1x_syncpt *sp, u32 thresh, long timeout,
		       u32 *value);
struct host1x_syncpt *host1x_syncpt_request(struct host1x_client *client,
					    unsigned long flags);
void host1x_syncpt_free(struct host1x_syncpt *sp);

struct host1x_syncpt_base *host1x_syncpt_get_base(struct host1x_syncpt *sp);
u32 host1x_syncpt_base_id(struct host1x_syncpt_base *base);

/*
 * subdevice probe infrastructure
 */

struct host1x_device;

/**
 * struct host1x_driver - host1x logical device driver
 * @driver: core driver
 * @subdevs: table of OF device IDs matching subdevices for this driver
 * @list: list node for the driver
 * @probe: called when the host1x logical device is probed
 * @remove: called when the host1x logical device is removed
 * @shutdown: called when the host1x logical device is shut down
 */
struct host1x_driver {
	struct device_driver driver;

	const struct of_device_id *subdevs;
	struct list_head list;

	int (*probe)(struct host1x_device *device);
	int (*remove)(struct host1x_device *device);
	void (*shutdown)(struct host1x_device *device);
};

static inline struct host1x_driver *
to_host1x_driver(struct device_driver *driver)
{
	return container_of(driver, struct host1x_driver, driver);
}

int host1x_driver_register_full(struct host1x_driver *driver,
				struct module *owner);
void host1x_driver_unregister(struct host1x_driver *driver);

#define host1x_driver_register(driver) \
	host1x_driver_register_full(driver, THIS_MODULE)

struct host1x_device {
	struct host1x_driver *driver;
	struct list_head list;
	struct device dev;

	struct mutex subdevs_lock;
	struct list_head subdevs;
	struct list_head active;

	struct mutex clients_lock;
	struct list_head clients;

	bool registered;

	struct device_dma_parameters dma_parms;
};

static inline struct host1x_device *to_host1x_device(struct device *dev)
{
	return container_of(dev, struct host1x_device, dev);
}

int host1x_device_init(struct host1x_device *device);
int host1x_device_exit(struct host1x_device *device);

int host1x_client_register(struct host1x_client *client);
int host1x_client_unregister(struct host1x_client *client);

int host1x_client_suspend(struct host1x_client *client);
int host1x_client_resume(struct host1x_client *client);

struct tegra_mipi_device;

struct tegra_mipi_device *tegra_mipi_request(struct device *device);
void tegra_mipi_free(struct tegra_mipi_device *device);
int tegra_mipi_enable(struct tegra_mipi_device *device);
int tegra_mipi_disable(struct tegra_mipi_device *device);
int tegra_mipi_calibrate(struct tegra_mipi_device *device);

#endif
