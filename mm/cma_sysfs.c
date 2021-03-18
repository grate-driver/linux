// SPDX-License-Identifier: GPL-2.0
/*
 * CMA SysFS Interface
 *
 * Copyright (c) 2021 Minchan Kim <minchan@kernel.org>
 */

#include <linux/cma.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "cma.h"

static struct cma_stat *cma_stats;

void cma_sysfs_alloc_pages_count(struct cma *cma, size_t count)
{
	spin_lock(&cma->stat->lock);
	cma->stat->nr_pages_succeeded += count;
	spin_unlock(&cma->stat->lock);
}

void cma_sysfs_fail_pages_count(struct cma *cma, size_t count)
{
	spin_lock(&cma->stat->lock);
	cma->stat->nr_pages_failed += count;
	spin_unlock(&cma->stat->lock);
}

#define CMA_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static struct kobject *cma_kobj;

static ssize_t alloc_pages_success_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	struct cma_stat *stat = container_of(kobj, struct cma_stat, kobj);

	return sysfs_emit(buf, "%lu\n", stat->nr_pages_succeeded);
}
CMA_ATTR_RO(alloc_pages_success);

static ssize_t alloc_pages_fail_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	struct cma_stat *stat = container_of(kobj, struct cma_stat, kobj);

	return sysfs_emit(buf, "%lu\n", stat->nr_pages_failed);
}
CMA_ATTR_RO(alloc_pages_fail);

static void cma_kobj_release(struct kobject *kobj)
{
	struct cma_stat *stat = container_of(kobj, struct cma_stat, kobj);

	kfree(stat);
}

static struct attribute *cma_attrs[] = {
	&alloc_pages_success_attr.attr,
	&alloc_pages_fail_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cma);

static struct kobj_type cma_ktype = {
	.release = cma_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = cma_groups
};

static int __init cma_sysfs_init(void)
{
	int i = 0;
	struct cma *cma;

	cma_kobj = kobject_create_and_add("cma", mm_kobj);
	if (!cma_kobj)
		return -ENOMEM;

	cma_stats = kmalloc_array(cma_area_count, sizeof(struct cma_stat),
				GFP_KERNEL|__GFP_ZERO);
	if (ZERO_OR_NULL_PTR(cma_stats))
		goto out;

	do {
		cma = &cma_areas[i];
		cma->stat = &cma_stats[i];
		spin_lock_init(&cma->stat->lock);
		if (kobject_init_and_add(&cma->stat->kobj, &cma_ktype,
					cma_kobj, "%s", cma->name)) {
			kobject_put(&cma->stat->kobj);
			goto out;
		}
	} while (++i < cma_area_count);

	return 0;
out:
	while (--i >= 0) {
		cma = &cma_areas[i];
		kobject_put(&cma->stat->kobj);
	}

	kfree(cma_stats);
	kobject_put(cma_kobj);

	return -ENOMEM;
}
subsys_initcall(cma_sysfs_init);
