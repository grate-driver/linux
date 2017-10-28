/*
 * Copyright (c) 2012-2015, NVIDIA Corporation.
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

#ifndef HOST1X_FIREWALL_H
#define HOST1X_FIREWALL_H

#define FW_ERR(fmt, args...) \
	pr_err("HOST1X firewall: %s: " fmt, __func__, ##args)

struct host1x;
struct host1x_bo;
struct host1x_job;
struct host1x_reloc;
struct device;

struct host1x_firewall {
	struct host1x_job *job;
	struct device *dev;

	unsigned int num_relocs;
	struct host1x_reloc *reloc;

	struct host1x_bo *cmdbuf;
	unsigned int offset;

	int syncpt_incrs;

	u32 *cmdbuf_base;
	u32 words;
	u32 class;
	u32 reg;
	u32 mask;
	u32 count;
};

int host1x_firewall_check_job(struct host1x *host, struct host1x_job *job,
			      struct device *dev);

int host1x_firewall_copy_gathers(struct host1x *host, struct host1x_job *job,
				 struct device *dev);

#endif
