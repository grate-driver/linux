/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2010 Google, Inc.
 * Author: Erik Gilling <konkers@android.com>
 *
 * Copyright (C) 2011-2013 NVIDIA Corporation
 */

#include "host1x.h"

static void
host1x_debug_write_to_seqfile(const char *str, size_t len, bool cont,
			      void *opaque)
{
	seq_write(opaque, str, len);
}

static int host1x_debug_show(struct seq_file *s, void *unused)
{
	struct host1x *host = s->private;
	struct host1x_dbg_output o;

	o.fn = host1x_debug_write_to_seqfile;
	o.opaque = s;

	host1x_debug_dump_channels(host, &o);
	host1x_debug_output(&o, "\n");

	host1x_debug_dump_syncpts(host, &o);
	host1x_debug_output(&o, "\n");

	host1x_debug_dump_mlocks(host, &o);
	host1x_debug_output(&o, "\n");

	return 0;
}

static int host1x_debug_status(struct inode *inode, struct file *file)
{
	return single_open(file, host1x_debug_show, inode->i_private);
}

static const struct file_operations host1x_debug_status_fops = {
	.open		= host1x_debug_status,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

int host1x_init_debug(struct host1x *host)
{
	spin_lock_init(&host->debug_lock);

	host->debugfs = debugfs_create_dir("tegra-host1x", NULL);

	debugfs_create_file("status", S_IRUGO, host->debugfs, host,
			    &host1x_debug_status_fops);

	return 0;
}

void host1x_deinit_debug(struct host1x *host)
{
	debugfs_remove_recursive(host->debugfs);
}

void host1x_debug_output(struct host1x_dbg_output *o, const char *fmt, ...)
{
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(o->buf, sizeof(o->buf), fmt, args);
	va_end(args);

	o->fn(o->buf, len, false, o->opaque);
}
EXPORT_SYMBOL(host1x_debug_output);

void host1x_debug_cont(struct host1x_dbg_output *o, const char *fmt, ...)
{
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(o->buf, sizeof(o->buf), fmt, args);
	va_end(args);

	o->fn(o->buf, len, true, o->opaque);
}
EXPORT_SYMBOL(host1x_debug_cont);
