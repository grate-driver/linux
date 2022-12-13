// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ASUS EC: DockRAM
 */

#include <asm/unaligned.h>
#include <linux/i2c.h>
#include <linux/mfd/asus-ec.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

struct dockram_ec_data {
	struct mutex ctl_lock;
	char ctl_data[DOCKRAM_ENTRY_BUFSIZE];
};

int asus_dockram_read(struct i2c_client *client, int reg, char *buf)
{
	int rc;

	memset(buf, 0, DOCKRAM_ENTRY_BUFSIZE);
	rc = i2c_smbus_read_i2c_block_data(client, reg, DOCKRAM_ENTRY_BUFSIZE,
					   buf);
	if (rc < 0)
		return rc;

	if (buf[0] > DOCKRAM_ENTRY_SIZE) {
		dev_err(&client->dev, "bad data len; buffer: %*ph; rc: %d\n",
			DOCKRAM_ENTRY_BUFSIZE, buf, rc);
		return -EPROTO;
	}

	dev_dbg(&client->dev, "got data; buffer: %*ph; rc: %d\n",
		DOCKRAM_ENTRY_BUFSIZE, buf, rc);

	return 0;
}
EXPORT_SYMBOL_GPL(asus_dockram_read);

int asus_dockram_write(struct i2c_client *client, int reg, const char *buf)
{
	if (buf[0] > DOCKRAM_ENTRY_SIZE)
		return -EINVAL;

	dev_dbg(&client->dev, "sending data; buffer: %*ph\n", buf[0] + 1, buf);

	return i2c_smbus_write_i2c_block_data(client, reg, buf[0] + 1, buf);
}
EXPORT_SYMBOL_GPL(asus_dockram_write);

int asus_dockram_access_ctl(struct i2c_client *client,
			    u64 *out, u64 mask, u64 xor)
{
	struct dockram_ec_data *priv = i2c_get_clientdata(client);
	char *buf = priv->ctl_data;
	u64 val;
	int ret;

	mutex_lock(&priv->ctl_lock);

	ret = asus_dockram_read(client, 0x0A, buf);
	if (ret < 0)
		goto unlock_exit;

	if (buf[0] != 8) {
		ret = -EPROTO;
		goto unlock_exit;
	}

	val = get_unaligned_le64(buf + 1);

	if (out)
		*out = val;

	if (mask || xor) {
		put_unaligned_le64((val & ~mask) ^ xor, buf + 1);
		ret = asus_dockram_write(client, 0x0A, buf);
	}

unlock_exit:
	mutex_unlock(&priv->ctl_lock);
	if (ret < 0)
		dev_err(&client->dev, "Failed to access control flags: %d\n",
			ret);
	return ret;
}
EXPORT_SYMBOL_GPL(asus_dockram_access_ctl);

static ssize_t dockram_read(struct file *filp, struct kobject *kobj,
			    struct bin_attribute *attr,
			    char *buf, loff_t off, size_t count)
{
	struct i2c_client *client = kobj_to_i2c_client(kobj);
	unsigned int reg;
	ssize_t n_read = 0;
	char *data;
	int ret;

	reg = off / DOCKRAM_ENTRY_SIZE;
	off %= DOCKRAM_ENTRY_SIZE;

	if (!count)
		return 0;

	data = kmalloc(DOCKRAM_ENTRY_BUFSIZE, GFP_KERNEL);

	while (reg < DOCKRAM_ENTRIES) {
		unsigned int len = DOCKRAM_ENTRY_SIZE - off;

		if (len > count)
			len = count;

		ret = asus_dockram_read(client, reg, data);
		if (ret < 0) {
			if (!n_read)
				n_read = ret;
			break;
		}

		memcpy(buf, data + 1 + off, len);
		n_read += len;

		if (len == count)
			break;

		count -= len;
		buf += len;
		off = 0;
		++reg;
	}

	kfree(data);
	return n_read;
}

static int dockram_write_one(struct i2c_client *client, int reg,
			     const char *buf, size_t count)
{
	struct dockram_ec_data *priv = i2c_get_clientdata(client);
	int ret;

	if (count > DOCKRAM_ENTRY_SIZE)
		return -EINVAL;

	mutex_lock(&priv->ctl_lock);

	priv->ctl_data[0] = (u8)count;
	memcpy(priv->ctl_data + 1, buf, count);
	ret = asus_dockram_write(client, reg, priv->ctl_data);

	mutex_unlock(&priv->ctl_lock);

	return ret;
}

static ssize_t dockram_write(struct file *filp, struct kobject *kobj,
			     struct bin_attribute *attr,
			     char *buf, loff_t off, size_t count)
{
	struct i2c_client *client = kobj_to_i2c_client(kobj);
	unsigned int reg;
	int ret;

	if (off % DOCKRAM_ENTRY_SIZE != 0)
		return -EINVAL;

	reg = off / DOCKRAM_ENTRY_SIZE;
	if (reg >= DOCKRAM_ENTRIES)
		return -EINVAL;

	ret = dockram_write_one(client, reg, buf, count);

	return ret < 0 ? ret : count;
}

static ssize_t control_reg_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	u64 val;
	int ret;

	ret = asus_dockram_access_ctl(client, &val, 0, 0);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%016llx\n", val);
}

static ssize_t control_reg_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	u64 val;
	int ret;

	ret = kstrtoull(buf, 16, &val);
	if (ret < 0)
		return ret;

	ret = asus_dockram_access_ctl(client, NULL, ~0ull, val);
	if (ret < 0)
		return ret;

	return count;
}

static BIN_ATTR_RW(dockram, DOCKRAM_ENTRIES * DOCKRAM_ENTRY_SIZE);
static DEVICE_ATTR_RW(control_reg);

static struct attribute *dockram_attrs[] = {
	&dev_attr_control_reg.attr,
	NULL
};

static struct bin_attribute *dockram_bin_attrs[] = {
	&bin_attr_dockram,
	NULL
};

static const struct attribute_group dockram_group = {
	.attrs = dockram_attrs,
	.bin_attrs = dockram_bin_attrs,
};

static int asus_dockram_probe(struct i2c_client *client)
{
	struct dockram_ec_data *priv;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_I2C_BLOCK)) {
		dev_err(&client->dev,
			"I2C bus is missing required SMBus block mode support\n");
		return -ENODEV;
	}

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	i2c_set_clientdata(client, priv);

	mutex_init(&priv->ctl_lock);

	return devm_device_add_group(&client->dev, &dockram_group);
}

static const struct of_device_id asus_dockram_ids[] = {
	{ .compatible = "asus,dockram" },
	{ }
};
MODULE_DEVICE_TABLE(of, asus_dockram_ids);

static struct i2c_driver asus_dockram_driver = {
	.driver	= {
		.name = "asus-dockram",
		.of_match_table = of_match_ptr(asus_dockram_ids),
	},
	.probe_new = asus_dockram_probe,
};
module_i2c_driver(asus_dockram_driver);

static void devm_put_device(void *dev)
{
	put_device(dev);
}

struct i2c_client *devm_asus_dockram_get(struct device *parent)
{
	struct i2c_client *dockram;
	int err = 0;

	dockram = of_get_i2c_device_by_phandle(parent, "asus,dockram", 0);
	if (IS_ERR(dockram))
		return dockram;

	if (!dockram->dev.driver)
		err = -EPROBE_DEFER;
	else if (dockram->dev.driver != &asus_dockram_driver.driver)
		err = -ENXIO;
	else
		err = devm_add_action(parent, devm_put_device, &dockram->dev);

	if (err)
		goto ret_release;

	return dockram;

ret_release:
	put_device(&dockram->dev);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(devm_asus_dockram_get);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer's dockram driver");
MODULE_LICENSE("GPL");
