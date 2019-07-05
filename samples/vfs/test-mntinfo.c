// SPDX-License-Identifier: GPL-2.0-or-later
/* Test the fsinfo() system call
 *
 * Copyright (C) 2020 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#define _GNU_SOURCE
#define _ATFILE_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sys/syscall.h>
#include <linux/fsinfo.h>
#include <linux/socket.h>
#include <linux/fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#ifndef __NR_fsinfo
#define __NR_fsinfo -1
#endif

static __attribute__((unused))
ssize_t fsinfo(int dfd, const char *filename,
	       struct fsinfo_params *params, size_t params_size,
	       void *result_buffer, size_t result_buf_size)
{
	return syscall(__NR_fsinfo, dfd, filename,
		       params, params_size,
		       result_buffer, result_buf_size);
}

static char tree_buf[4096];
static char bar_buf[4096];
static unsigned int children_list_interval;

/*
 * Get an fsinfo attribute in a statically allocated buffer.
 */
static void get_attr(unsigned int mnt_id, unsigned int attr, unsigned int Nth,
		     void *buf, size_t buf_size)
{
	struct fsinfo_params params = {
		.flags		= FSINFO_FLAGS_QUERY_MOUNT,
		.request	= attr,
		.Nth		= Nth,
	};
	char file[32];
	long ret;

	sprintf(file, "%u", mnt_id);

	memset(buf, 0xbd, buf_size);

	ret = fsinfo(AT_FDCWD, file, &params, sizeof(params), buf, buf_size);
	if (ret == -1) {
		fprintf(stderr, "mount-%s: %m\n", file);
		exit(1);
	}
}

/*
 * Get an fsinfo attribute in a dynamically allocated buffer.
 */
static void *get_attr_alloc(unsigned int mnt_id, unsigned int attr,
			    unsigned int Nth, size_t *_size)
{
	struct fsinfo_params params = {
		.flags		= FSINFO_FLAGS_QUERY_MOUNT,
		.request	= attr,
		.Nth		= Nth,
	};
	size_t buf_size = 4096;
	char file[32];
	void *r;
	long ret;

	sprintf(file, "%u", mnt_id);

	for (;;) {
		r = malloc(buf_size);
		if (!r) {
			perror("malloc");
			exit(1);
		}
		memset(r, 0xbd, buf_size);

		ret = fsinfo(AT_FDCWD, file, &params, sizeof(params), r, buf_size);
		if (ret == -1) {
			fprintf(stderr, "mount-%s: %x,%x,%x %m\n",
				file, params.request, params.Nth, params.Mth);
			exit(1);
		}

		if (ret <= buf_size) {
			*_size = ret;
			break;
		}
		buf_size = (ret + 4096 - 1) & ~(4096 - 1);
	}

	return r;
}

/*
 * Display a mount and then recurse through its children.
 */
static void display_mount(unsigned int mnt_id, unsigned int depth, char *path)
{
	struct fsinfo_mount_topology top;
	struct fsinfo_mount_child child;
	struct fsinfo_mount_info info;
	struct fsinfo_ids ids;
	void *children;
	unsigned int d;
	size_t ch_size, p_size;
	char dev[64];
	int i, n, s;

	get_attr(mnt_id, FSINFO_ATTR_MOUNT_TOPOLOGY, 0, &top, sizeof(top));
	get_attr(mnt_id, FSINFO_ATTR_MOUNT_INFO, 0, &info, sizeof(info));
	get_attr(mnt_id, FSINFO_ATTR_IDS, 0, &ids, sizeof(ids));
	if (depth > 0)
		printf("%s", tree_buf);

	s = strlen(path);
	printf("%s", !s ? "\"\"" : path);
	if (!s)
		s += 2;
	s += depth;
	if (s < 38)
		s = 38 - s;
	else
		s = 1;
	printf("%*.*s", s, s, "");

	sprintf(dev, "%x:%x", ids.f_dev_major, ids.f_dev_minor);
	printf("%10u %8x %2x %x %5s %s",
	       info.mnt_id,
	       (info.sb_changes +
		info.sb_notifications +
		info.mnt_attr_changes +
		info.mnt_topology_changes +
		info.mnt_subtree_notifications),
	       info.attr, top.propagation,
	       dev, ids.f_fs_name);
	putchar('\n');

	children = get_attr_alloc(mnt_id, FSINFO_ATTR_MOUNT_CHILDREN, 0, &ch_size);
	n = ch_size / children_list_interval - 1;

	bar_buf[depth + 1] = '|';
	if (depth > 0) {
		tree_buf[depth - 4 + 1] = bar_buf[depth - 4 + 1];
		tree_buf[depth - 4 + 2] = ' ';
	}

	tree_buf[depth + 0] = ' ';
	tree_buf[depth + 1] = '\\';
	tree_buf[depth + 2] = '_';
	tree_buf[depth + 3] = ' ';
	tree_buf[depth + 4] = 0;
	d = depth + 4;

	memset(&child, 0, sizeof(child));
	for (i = 0; i < n; i++) {
		void *p = children + i * children_list_interval;

		if (sizeof(child) >= children_list_interval)
			memcpy(&child, p, children_list_interval);
		else
			memcpy(&child, p, sizeof(child));

		if (i == n - 1)
			bar_buf[depth + 1] = ' ';
		path = get_attr_alloc(child.mnt_id, FSINFO_ATTR_MOUNT_POINT,
				      0, &p_size);
		display_mount(child.mnt_id, d, path + 1);
		free(path);
	}

	free(children);
	if (depth > 0) {
		tree_buf[depth - 4 + 1] = '\\';
		tree_buf[depth - 4 + 2] = '_';
	}
	tree_buf[depth] = 0;
}

/*
 * Find the ID of whatever is at the nominated path.
 */
static unsigned int lookup_mnt_by_path(const char *path)
{
	struct fsinfo_mount_info mnt;
	struct fsinfo_params params = {
		.flags		= FSINFO_FLAGS_QUERY_PATH,
		.request	= FSINFO_ATTR_MOUNT_INFO,
	};

	if (fsinfo(AT_FDCWD, path, &params, sizeof(params), &mnt, sizeof(mnt)) == -1) {
		perror(path);
		exit(1);
	}

	return mnt.mnt_id;
}

/*
 * Determine the element size for the mount child list.
 */
static unsigned int query_list_element_size(int mnt_id, unsigned int attr)
{
	struct fsinfo_attribute_info attr_info;

	get_attr(mnt_id, FSINFO_ATTR_FSINFO_ATTRIBUTE_INFO, attr,
		 &attr_info, sizeof(attr_info));
	return attr_info.size;
}

/*
 *
 */
int main(int argc, char **argv)
{
	unsigned int mnt_id;
	char *path;
	bool use_mnt_id = false;
	int opt;

	while ((opt = getopt(argc, argv, "m"))) {
		switch (opt) {
		case 'm':
			use_mnt_id = true;
			continue;
		}
		break;
	}

	argc -= optind;
	argv += optind;

	switch (argc) {
	case 0:
		mnt_id = lookup_mnt_by_path("/");
		path = "ROOT";
		break;
	case 1:
		path = argv[0];
		if (use_mnt_id) {
			mnt_id = strtoul(argv[0], NULL, 0);
			break;
		}

		mnt_id = lookup_mnt_by_path(argv[0]);
		break;
	default:
		printf("Format: test-mntinfo\n");
		printf("Format: test-mntinfo <path>\n");
		printf("Format: test-mntinfo -m <mnt_id>\n");
		exit(2);
	}

	children_list_interval =
		query_list_element_size(mnt_id, FSINFO_ATTR_MOUNT_CHILDREN);

	printf("MOUNT                                 MOUNT ID   CHANGE#  AT P DEV   TYPE\n");
	printf("------------------------------------- ---------- -------- -- - ----- --------\n");
	display_mount(mnt_id, 0, path);
	return 0;
}
