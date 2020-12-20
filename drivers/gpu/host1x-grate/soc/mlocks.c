/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/err.h>

static int host1x_soc_init_mlocks(struct host1x *host)
{
	idr_init(&host->mlocks);
	spin_lock_init(&host->mlocks_lock);

	return 0;
}

static void host1x_soc_deinit_mlocks(struct host1x *host)
{
	/* shouldn't happen, all mlocks must be released at this point */
	WARN_ON(!idr_is_empty(&host->mlocks));

	idr_destroy(&host->mlocks);
}

static struct host1x_mlock *
host1x_soc_mlock_request(struct host1x *host, struct device *dev)
{
	struct host1x_mlock *mlock;
	int ret;

	mlock = kzalloc(sizeof(*mlock), GFP_KERNEL);
	if (!mlock)
		return ERR_PTR(-ENOMEM);

	idr_preload(GFP_KERNEL);
	spin_lock(&host->mlocks_lock);

	ret = idr_alloc(&host->mlocks, mlock, 0, HOST1X_MLOCKS_NUM,
			GFP_ATOMIC);

	spin_unlock(&host->mlocks_lock);
	idr_preload_end();

	if (ret < 0) {
		kfree(mlock);
		return ERR_PTR(ret);
	}

	kref_init(&mlock->refcount);
	mlock->host = host;
	mlock->dev = dev;
	mlock->id = ret;

	return mlock;
}

static void host1x_soc_mlock_release(struct kref *kref)
{
	struct host1x_mlock *mlock = container_of(kref, struct host1x_mlock,
						  refcount);
	struct host1x *host = mlock->host;

	spin_lock(&host->mlocks_lock);
	idr_remove(&host->mlocks, mlock->id);
	spin_unlock(&host->mlocks_lock);

	kfree(mlock);
}

static void
host1x_soc_mlock_unlock_channel(struct host1x_channel *chan)
{
#if HOST1X_HW < 6
	struct host1x *host = chan->host;
	unsigned int i;
	u32 owner;

	for (i = 0; i < HOST1X_MLOCKS_NUM; i++) {
		owner = host1x_hw_mlock_owner(host, i);

		if (HOST1X_SYNC_MLOCK_OWNER_CH_OWNS_V(owner) &&
		    HOST1X_SYNC_MLOCK_OWNER_CHID_V(owner) == chan->id)
			host1x_hw_mlock_unlock(host, i);
	}
#endif
}

static void
host1x_soc_dump_mlock_by_id(struct host1x_dbg_output *o,
			    struct host1x *host,
			    unsigned int id)
{
#if HOST1X_HW < 6
	u32 owner = host1x_hw_mlock_owner(host, id);
	struct host1x_mlock *mlock;
	char user_name[256];

	spin_lock(&host->mlocks_lock);

	mlock = idr_find(&host->mlocks, id);
	if (mlock)
		snprintf(user_name, ARRAY_SIZE(user_name),
			 "%s", dev_name(mlock->dev));

	spin_unlock(&host->mlocks_lock);

	if (HOST1X_SYNC_MLOCK_OWNER_CH_OWNS_V(owner))
		host1x_debug_output(o, "mlock %u: locked by channel %u, %s\n",
				    id, HOST1X_SYNC_MLOCK_OWNER_CHID_V(owner),
				    mlock ? user_name : "unused");
	else if (HOST1X_SYNC_MLOCK_OWNER_CPU_OWNS_V(owner))
		host1x_debug_output(o, "mlock %u: locked by cpu, %s\n",
				    id, mlock ? user_name : "unused");
	else
		host1x_debug_output(o, "mlock %u: unlocked, %s\n",
				    id, mlock ? user_name : "unused");
#endif
}

static void
host1x_soc_dump_mlocks(struct host1x_dbg_output *o, struct host1x *host)
{
	unsigned int i;

	for (i = 0; i < HOST1X_MLOCKS_NUM; i++)
		host1x_soc_dump_mlock_by_id(o, host, i);
}
