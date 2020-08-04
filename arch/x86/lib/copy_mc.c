// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2016-2020 Intel Corporation. All rights reserved. */

#include <linux/jump_label.h>
#include <linux/uaccess.h>
#include <linux/export.h>
#include <linux/string.h>
#include <linux/types.h>

static DEFINE_STATIC_KEY_FALSE(copy_mc_fragile_key);

void enable_copy_mc_fragile(void)
{
	static_branch_inc(&copy_mc_fragile_key);
}

/**
 * copy_mc_to_kernel - memory copy that that handles source exceptions
 *
 * @dst:	destination address
 * @src:	source address
 * @cnt:	number of bytes to copy
 *
 * Call into the 'fragile' version on systems that have trouble
 * actually do machine check recovery. Everyone else can just
 * use copy_mc_generic().
 *
 * Return 0 for success, or number of bytes not copied if there was an
 * exception.
 */
unsigned long __must_check
copy_mc_to_kernel(void *dst, const void *src, unsigned cnt)
{
	if (static_branch_unlikely(&copy_mc_fragile_key))
		return copy_mc_fragile(dst, src, cnt);
	return copy_mc_generic(dst, src, cnt);
}
EXPORT_SYMBOL_GPL(copy_mc_to_kernel);

/*
 * Similar to copy_user_handle_tail, probe for the write fault point, or
 * source exception point.
 */
__visible notrace unsigned long
copy_mc_fragile_handle_tail(char *to, char *from, unsigned len)
{
	for (; len; --len, to++, from++)
		if (copy_mc_fragile(to, from, 1))
			break;
	return len;
}

unsigned long __must_check
copy_mc_to_user(void *to, const void *from, unsigned len)
{
	unsigned long ret;

	__uaccess_begin();
	if (static_branch_unlikely(&copy_mc_fragile_key))
		ret = copy_mc_fragile(to, from, len);
	ret = copy_mc_generic(to, from, len);
	__uaccess_end();
	return ret;
}
