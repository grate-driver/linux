#ifndef __MISC_ASUS_EC_H
#define __MISC_ASUS_EC_H

#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

struct i2c_client;

/* dockram comm */

int asus_dockram_read(struct i2c_client *client, int reg, char *buf);
int asus_dockram_write(struct i2c_client *client, int reg, const char *buf);
int asus_dockram_access_ctl(struct i2c_client *client,
			    u64 *out, u64 mask, u64 xor);
struct i2c_client *devm_asus_dockram_get(struct device *parent);

#define DOCKRAM_ENTRIES 0x100
#define DOCKRAM_ENTRY_SIZE 32
#define DOCKRAM_ENTRY_BUFSIZE (DOCKRAM_ENTRY_SIZE + 1)

/* EC public API */

struct asusec_info
{
	const char			*name;
	const char			*model;
	struct i2c_client		*dockram;
	struct workqueue_struct		*wq;
	struct blocking_notifier_head	 notify_list;
};

#define ASUSEC_OBF_MASK		0x01
#define ASUSEC_KEY_MASK		0x04
#define ASUSEC_KBC_MASK		0x08
#define ASUSEC_AUX_MASK		0x20
#define ASUSEC_SCI_MASK		0x40
#define ASUSEC_SMI_MASK		0x80

static inline const struct asusec_info *asusec_cell_to_ec(
	struct platform_device *pdev)
{
	return dev_get_drvdata(pdev->dev.parent);
}

static inline int asusec_register_notifier(const struct asusec_info *ec,
			     struct notifier_block *nb)
{
	struct asusec_info *ec_rw = (void *)ec;

	return blocking_notifier_chain_register(&ec_rw->notify_list, nb);
}

static inline int asusec_unregister_notifier(const struct asusec_info *ec,
			       struct notifier_block *nb)
{
	struct asusec_info *ec_rw = (void *)ec;

	return blocking_notifier_chain_unregister(&ec_rw->notify_list, nb);
}

static inline int asusec_get_ctl(const struct asusec_info *ec, u64 *out)
{
	return asus_dockram_access_ctl(ec->dockram, out, 0, 0);
}

static inline int asusec_update_ctl(const struct asusec_info *ec,
				    u64 mask, u64 xor)
{
	return asus_dockram_access_ctl(ec->dockram, NULL, mask, xor);
}

static inline int asusec_set_ctl_bits(const struct asusec_info *ec, u64 mask)
{
	return asus_dockram_access_ctl(ec->dockram, NULL, mask, mask);
}

static inline int asusec_clear_ctl_bits(const struct asusec_info *ec, u64 mask)
{
	return asus_dockram_access_ctl(ec->dockram, NULL, mask, 0);
}

int asusec_signal_request(const struct asusec_info *ec);
int asusec_i2c_command(const struct asusec_info *ec, u16 data);

#endif /* __MISC_ASUS_EC_H */
