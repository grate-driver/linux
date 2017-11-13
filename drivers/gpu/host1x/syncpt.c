/*
 * Tegra host1x Syncpoints
 *
 * Copyright (c) 2010-2015, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/slab.h>

#include <trace/events/host1x.h>

#include "dev.h"
#include "debug.h"

#define SYNCPT_CHECK_PERIOD	(2 * HZ)
#define MAX_STUCK_CHECK_COUNT	15

static struct host1x_syncpt_base * host1x_get_unused_base(struct host1x *host)
{
	unsigned int index;

	index = find_first_zero_bit(host->requested_bases,
				    host1x_syncpt_nb_bases(host));
	if (index >= host1x_syncpt_nb_bases(host))
		return NULL;

	set_bit(index, host->requested_bases);

	return host->bases + index;
}

static struct host1x_syncpt * host1x_get_unused_syncpt(struct host1x *host)
{
	unsigned int index;

	index = find_first_zero_bit(host->requested_syncpts,
				    host1x_syncpt_nb_pts(host));
	if (index >= host1x_syncpt_nb_pts(host))
		return NULL;

	set_bit(index, host->requested_syncpts);

	return host->syncpts + index;
}

/**
 * host1x_syncpt_id() - retrieve syncpoint ID
 * @sp: host1x syncpoint
 *
 * Given a pointer to a struct host1x_syncpt, retrieves its ID. This ID is
 * often used as a value to program into registers that control how hardware
 * blocks interact with syncpoints.
 */
u32 host1x_syncpt_id(struct host1x_syncpt *sp)
{
	return sp->id;
}
EXPORT_SYMBOL(host1x_syncpt_id);

/**
 * host1x_syncpt_incr_max() - update the value sent to hardware
 * @sp: host1x syncpoint
 * @incrs: number of increments
 */
u32 host1x_syncpt_incr_max(struct host1x_syncpt *sp, u32 incrs)
{
	return (u32)atomic_add_return(incrs, &sp->max_val);
}
EXPORT_SYMBOL(host1x_syncpt_incr_max);

/*
 * Write cached syncpoint and waitbase values to hardware.
 */
void host1x_syncpt_restore(struct host1x *host)
{
	struct host1x_syncpt *sp = host->syncpts;
	unsigned int i;

	for (i = 0; i < host1x_syncpt_nb_pts(host); i++, sp++) {
		if (sp->client_managed)
			continue;

		host1x_hw_syncpt_restore(host, sp->id,
					 host1x_syncpt_read_min(sp));

		if (sp->base)
			host1x_hw_syncpt_restore_wait_base(host, sp->base->id,
							   sp->base->value);
	}
}

/*
 * Returns true if syncpoint min == max, which means that there are no
 * outstanding operations.
 */
static bool host1x_syncpt_idle(struct host1x_syncpt *sp)
{
	u32 min = atomic_read(&sp->min_val);
	u32 max = atomic_read(&sp->max_val);

	return (min == max);
}

/*
 * Update the cached syncpoint and waitbase values by reading them
 * from the registers.
 */
void host1x_syncpt_save(struct host1x *host)
{
	struct host1x_syncpt *sp = host->syncpts;
	unsigned int i;

	for (i = 0; i < host1x_syncpt_nb_pts(host); i++, sp++) {
		if (sp->client_managed || host1x_syncpt_idle(sp))
			continue;

		host1x_syncpt_load(sp);

		if (sp->base)
			host1x_syncpt_load_wait_base(sp);
	}
}

/*
 * Check sync point sanity. If max is larger than min, there have too many
 * sync point increments.
 *
 * Client managed sync point are not tracked.
 * */
static bool host1x_syncpt_check_max(struct host1x_syncpt *sp, u32 real)
{
	u32 max;

	if (sp->client_managed)
		return true;

	max = host1x_syncpt_read_max(sp);

	return (s32)(max - real) >= 0;
}

/*
 * Updates the cached syncpoint value by reading a new value from the hardware
 * register
 */
u32 host1x_syncpt_load(struct host1x_syncpt *sp)
{
	struct host1x *host = sp->host;
	u32 old, live;

	/* Loop in case there's a race writing to min_val */
	do {
		old = host1x_syncpt_read_min(sp);
		live = host1x_hw_syncpt_load(host, sp->id);
	} while ((u32)atomic_cmpxchg(&sp->min_val, old, live) != old);

	if (!host1x_syncpt_check_max(sp, live))
		dev_err(host->dev, "%s failed: id=%u, min=%d, max=%d\n",
			__func__, sp->id,
			host1x_syncpt_read_min(sp),
			host1x_syncpt_read_max(sp));

	trace_host1x_syncpt_load_min(sp->id, live);

	return live;
}

/*
 * Get the current syncpoint base
 */
u32 host1x_syncpt_load_wait_base(struct host1x_syncpt *sp)
{
	sp->base->value = host1x_hw_syncpt_load_wait_base(sp->host,
							  sp->base->id);

	return sp->base->value;
}

/**
 * host1x_syncpt_incr() - increment syncpoint value from CPU, updating cache
 * @sp: host1x syncpoint
 */
int host1x_syncpt_incr(struct host1x_syncpt *sp)
{
	/* increment shadow copy */
	atomic_inc(&sp->max_val);
	atomic_inc(&sp->min_val);

	/* increment HW */
	host1x_hw_syncpt_cpu_incr(sp->host, sp->id);

	return 0;
}
EXPORT_SYMBOL(host1x_syncpt_incr);

/*
 * Updated sync point form hardware, and returns true if syncpoint is expired,
 * false if we may need to wait
 */
static bool syncpt_load_min_is_expired(struct host1x_syncpt *sp, u32 thresh)
{
	host1x_syncpt_load(sp);

	return host1x_syncpt_is_expired(sp, thresh);
}

/**
 * host1x_syncpt_wait() - wait for a syncpoint to reach a given value
 * @sp: host1x syncpoint
 * @thresh: threshold
 * @timeout: maximum time to wait for the syncpoint to reach the given value
 * @value: return location for the syncpoint value
 */
int host1x_syncpt_wait(struct host1x_syncpt *sp, u32 thresh, long timeout,
		       u32 *value)
{
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wq);
	void *ref;
	struct host1x_waitlist *waiter;
	int err = 0, check_count = 0;
	u32 val;

	if (value)
		*value = 0;

	/* first check cache */
	if (host1x_syncpt_is_expired(sp, thresh)) {
		if (value)
			*value = host1x_syncpt_load(sp);

		return 0;
	}

	/* try to read from register */
	val = host1x_syncpt_load(sp);
	if (host1x_syncpt_is_expired(sp, thresh)) {
		if (value)
			*value = val;

		return 0;
	}

	if (!timeout)
		return -EAGAIN;

	/* allocate a waiter */
	waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
	if (!waiter)
		return -ENOMEM;

	/* schedule a wakeup when the syncpoint value is reached */
	host1x_intr_add_action(sp->host, sp->id, thresh,
			       HOST1X_INTR_ACTION_WAKEUP_INTERRUPTIBLE,
			       &wq, waiter, &ref);

	err = -EAGAIN;
	/* Caller-specified timeout may be impractically low */
	if (timeout < 0)
		timeout = LONG_MAX;

	/* wait for the syncpoint, or timeout, or signal */
	while (timeout) {
		long check = min_t(long, SYNCPT_CHECK_PERIOD, timeout);
		int remain;

		remain = wait_event_interruptible_timeout(wq,
				syncpt_load_min_is_expired(sp, thresh),
				check);
		if (remain > 0 || host1x_syncpt_is_expired(sp, thresh)) {
			if (value)
				*value = host1x_syncpt_load(sp);

			err = 0;

			break;
		}

		if (remain < 0) {
			err = remain;
			break;
		}

		timeout -= check;

		if (timeout && check_count <= MAX_STUCK_CHECK_COUNT) {
			dev_warn(sp->host->dev,
				"%s: syncpoint %u stuck waiting %d, timeout=%ld\n",
				 current->comm, sp->id, thresh, timeout);

			host1x_debug_dump_syncpts(sp->host);

			if (check_count == MAX_STUCK_CHECK_COUNT)
				host1x_debug_dump(sp->host);

			check_count++;
		}
	}

	host1x_intr_put_ref(sp->host, sp->id, ref);

	return err;
}
EXPORT_SYMBOL(host1x_syncpt_wait);

/*
 * Returns true if syncpoint is expired, false if we may need to wait
 */
bool host1x_syncpt_is_expired(struct host1x_syncpt *sp, u32 thresh)
{
	u32 current_val;
	u32 future_val;

	current_val = (u32)atomic_read(&sp->min_val);
	future_val = (u32)atomic_read(&sp->max_val);

	/* Note the use of unsigned arithmetic here (mod 1<<32).
	 *
	 * c = current_val = min_val	= the current value of the syncpoint.
	 * t = thresh			= the value we are checking
	 * f = future_val  = max_val	= the value c will reach when all
	 *				  outstanding increments have completed.
	 *
	 * Note that c always chases f until it reaches f.
	 *
	 * Dtf = (f - t)
	 * Dtc = (c - t)
	 *
	 *  Consider all cases:
	 *
	 *	A) .....c..t..f.....	Dtf < Dtc	need to wait
	 *	B) .....c.....f..t..	Dtf > Dtc	expired
	 *	C) ..t..c.....f.....	Dtf > Dtc	expired	   (Dct very large)
	 *
	 *  Any case where f==c: always expired (for any t).	Dtf == Dcf
	 *  Any case where t==c: always expired (for any f).	Dtf >= Dtc (because Dtc==0)
	 *  Any case where t==f!=c: always wait.		Dtf <  Dtc (because Dtf==0,
	 *							Dtc!=0)
	 *
	 *  Other cases:
	 *
	 *	A) .....t..f..c.....	Dtf < Dtc	need to wait
	 *	A) .....f..c..t.....	Dtf < Dtc	need to wait
	 *	A) .....f..t..c.....	Dtf > Dtc	expired
	 *
	 *   So:
	 *	   Dtf >= Dtc implies EXPIRED	(return true)
	 *	   Dtf <  Dtc implies WAIT	(return false)
	 *
	 * Note: If t is expired then we *cannot* wait on it. We would wait
	 * forever (hang the system).
	 *
	 * Note: do NOT get clever and remove the -thresh from both sides. It
	 * is NOT the same.
	 *
	 * If future value is zero, we have a client managed sync point. In that
	 * case we do a direct comparison.
	 */
	if (!sp->client_managed)
		return future_val - thresh >= current_val - thresh;
	else
		return (s32)(current_val - thresh) >= 0;
}

/* remove a wait pointed to by patch_addr */
int host1x_syncpt_patch_wait(struct host1x_syncpt *sp, void *patch_addr)
{
	return host1x_hw_syncpt_patch_wait(sp->host, sp, patch_addr);
}

int host1x_syncpt_init(struct host1x *host)
{
	struct host1x_syncpt_base *bases;
	struct host1x_syncpt *syncpts;
	unsigned int i;

	syncpts = devm_kcalloc(host->dev, host1x_syncpt_nb_pts(host),
			       sizeof(*syncpts), GFP_KERNEL);
	if (!syncpts)
		return -ENOMEM;

	bases = devm_kcalloc(host->dev, host1x_syncpt_nb_bases(host),
			      sizeof(*bases), GFP_KERNEL);
	if (!bases)
		return -ENOMEM;

	host->requested_syncpts =
		devm_kcalloc(host->dev,
			     BITS_TO_LONGS(host1x_syncpt_nb_pts(host)),
			     sizeof(unsigned long), GFP_KERNEL);
	if (!host->requested_syncpts)
		return -ENOMEM;

	host->requested_bases =
		devm_kcalloc(host->dev,
			     BITS_TO_LONGS(host1x_syncpt_nb_bases(host)),
			     sizeof(unsigned long), GFP_KERNEL);
	if (!host->requested_bases)
		return -ENOMEM;

	for (i = 0; i < host1x_syncpt_nb_pts(host); i++) {
		syncpts[i].id = i;
		syncpts[i].host = host;

		/*
		 * Unassign syncpt from channels for purposes of Tegra186
		 * syncpoint protection. This prevents any channel from
		 * accessing it until it is reassigned.
		 */
		host1x_hw_firewall_syncpt_unassign(host, &syncpts[i]);
	}

	for (i = 0; i < host1x_syncpt_nb_bases(host); i++)
		bases[i].id = i;

	sema_init(&host->syncpt_base_sema, host1x_syncpt_nb_bases(host));
	sema_init(&host->syncpt_sema, host1x_syncpt_nb_pts(host));
	mutex_init(&host->syncpt_mutex);
	host->syncpts = syncpts;
	host->bases = bases;

	host1x_hw_firewall_enable_syncpt_protection(host);
	host1x_syncpt_restore(host);

	return 0;
}

/**
 * host1x_syncpt_request() - request a syncpoint
 * @client: client requesting the syncpoint
 * @flags: flags
 *
 * host1x client drivers can use this function to allocate a syncpoint for
 * subsequent use. A syncpoint returned by this function will be reserved for
 * use by the client exclusively. When no longer using a syncpoint, a host1x
 * client driver needs to release it using host1x_syncpt_put().
 */
struct host1x_syncpt *host1x_syncpt_request(struct host1x_client *client,
					    unsigned long flags)
{
	struct host1x *host = dev_get_drvdata(client->parent->parent);
	struct host1x_syncpt_base *base = NULL;
	struct host1x_syncpt *sp;
	bool blocking = !!(flags & HOST1X_SYNCPT_REQUEST_BLOCKING);
	bool managed  = !!(flags & HOST1X_SYNCPT_CLIENT_MANAGED);
	bool get_base = !!(flags & HOST1X_SYNCPT_HAS_BASE);
	const char *name;
	int err;

	name = kasprintf(GFP_KERNEL, "%s - %s%s",
			 current->comm, dev_name(client->dev),
			 managed ? " client-managed" : "");
	if (!name)
		return ERR_PTR(-ENOMEM);

	if (blocking) {
		err = down_interruptible(&host->syncpt_sema);
		if (err)
			goto err_free_name;

		if (get_base) {
			err = down_interruptible(&host->syncpt_base_sema);
			if (err)
				goto err_up_syncpt;
		}
	} else {
		err = down_trylock(&host->syncpt_sema);
		if (err)
			goto err_free_name;

		if (get_base) {
			err = down_trylock(&host->syncpt_base_sema);
			if (err)
				goto err_up_syncpt;
		}
	}

	mutex_lock(&host->syncpt_mutex);
	sp = host1x_get_unused_syncpt(host);

	if (get_base)
		base = host1x_get_unused_base(host);
	mutex_unlock(&host->syncpt_mutex);

	kref_init(&sp->refcount);
	sp->client_managed = managed;
	sp->base = base;
	sp->name = name;

	return sp;

err_up_syncpt:
	up(&host->syncpt_sema);

err_free_name:
	kfree(name);

	return ERR_PTR(err);
}
EXPORT_SYMBOL(host1x_syncpt_request);

void host1x_syncpt_deinit(struct host1x *host)
{
	unsigned int i;

	mutex_lock(&host->syncpt_mutex);

	if (!bitmap_empty(host->requested_syncpts, host1x_syncpt_nb_pts(host)))
		dev_warn(host->dev, "Syncpoint is in-use\n");

	if (!bitmap_empty(host->requested_bases, host1x_syncpt_nb_bases(host)))
		dev_warn(host->dev, "Syncpoint base is in-use\n");

	mutex_unlock(&host->syncpt_mutex);

	for (i = 0; i < host1x_syncpt_nb_pts(host); i++)
		down(&host->syncpt_sema);

	for (i = 0; i < host1x_syncpt_nb_bases(host); i++)
		down(&host->syncpt_base_sema);
}

/**
 * host1x_syncpt_read_max() - read maximum syncpoint value
 * @sp: host1x syncpoint
 *
 * The maximum syncpoint value indicates how many operations there are in
 * queue, either in channel or in a software thread.
 */
u32 host1x_syncpt_read_max(struct host1x_syncpt *sp)
{
	return (u32)atomic_read(&sp->max_val);
}
EXPORT_SYMBOL(host1x_syncpt_read_max);

/**
 * host1x_syncpt_read_min() - read minimum syncpoint value
 * @sp: host1x syncpoint
 *
 * The minimum syncpoint value is a shadow of the current sync point value in
 * hardware.
 */
u32 host1x_syncpt_read_min(struct host1x_syncpt *sp)
{
	return (u32)atomic_read(&sp->min_val);
}
EXPORT_SYMBOL(host1x_syncpt_read_min);

/**
 * host1x_syncpt_read() - read the current syncpoint value
 * @sp: host1x syncpoint
 */
u32 host1x_syncpt_read(struct host1x_syncpt *sp)
{
	return host1x_syncpt_load(sp);
}
EXPORT_SYMBOL(host1x_syncpt_read);

unsigned int host1x_syncpt_nb_pts(struct host1x *host)
{
	return host->info->nb_pts;
}

unsigned int host1x_syncpt_nb_bases(struct host1x *host)
{
	return host->info->nb_bases;
}

unsigned int host1x_syncpt_nb_mlocks(struct host1x *host)
{
	return host->info->nb_mlocks;
}

/**
 * host1x_syncpt_get_by_id() - obtain a syncpoint by ID
 * @host: host1x controller
 * @id: syncpoint ID
 */
struct host1x_syncpt *host1x_syncpt_get_by_id(struct host1x *host, u32 id)
{
	if (id >= host->info->nb_pts)
		return NULL;

	return host->syncpts + id;
}
EXPORT_SYMBOL(host1x_syncpt_get_by_id);

/**
 * host1x_syncpt_get_base() - obtain the wait base associated with a syncpoint
 * @sp: host1x syncpoint
 */
struct host1x_syncpt_base *host1x_syncpt_get_base(struct host1x_syncpt *sp)
{
	return sp->base;
}
EXPORT_SYMBOL(host1x_syncpt_get_base);

/**
 * host1x_syncpt_base_id() - retrieve the ID of a syncpoint wait base
 * @base: host1x syncpoint wait base
 */
u32 host1x_syncpt_base_id(struct host1x_syncpt_base *base)
{
	return base->id;
}
EXPORT_SYMBOL(host1x_syncpt_base_id);

/**
 * host1x_syncpt_get() - reference a requested syncpoint
 * @sp: host1x syncpoint
 *
 * Bump syncpoints reference counter.
 */
struct host1x_syncpt * host1x_syncpt_get(struct host1x_syncpt *sp)
{
	kref_get(&sp->refcount);

	return sp;
}
EXPORT_SYMBOL(host1x_syncpt_get);

/*
 * Release a syncpoint previously allocated using host1x_syncpt_request().
 * Note that client drivers must ensure that the syncpoint doesn't remain
 * under the control of hardware, otherwise two clients may end up trying
 * to access the same syncpoint concurrently.
 */
static void release_syncpoint(struct kref *kref)
{
	struct host1x_syncpt *sp = container_of(kref, struct host1x_syncpt,
						refcount);
	struct host1x *host = sp->host;
	bool up_base = false;
	u32 value;

	mutex_lock(&host->syncpt_mutex);

	kfree(sp->name);

	if (sp->client_managed) {
		sp->client_managed = false;

		/* sync cached values with HW */
		value = host1x_hw_syncpt_load(host, sp->id);
		atomic_set(&sp->min_val, value);
		atomic_set(&sp->max_val, value);
	}

	clear_bit(sp->id, host->requested_syncpts);

	if (sp->base) {
		clear_bit(sp->base->id, host->requested_bases);
		up_base = true;
	}

	mutex_unlock(&host->syncpt_mutex);

	if (up_base)
		up(&host->syncpt_base_sema);

	up(&host->syncpt_sema);
}

/**
 * host1x_syncpt_put() - unreference a requested syncpoint
 * @sp: host1x syncpoint
 *
 * Unreference syncpoint, freeing it when refcount drops to 0.
 */
void host1x_syncpt_put(struct host1x_syncpt *sp)
{
	kref_put(&sp->refcount, release_syncpoint);
}
EXPORT_SYMBOL(host1x_syncpt_put);
