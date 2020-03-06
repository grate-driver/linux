// SPDX-License-Identifier: GPL-2.0
/* Filesystem information query
 *
 * Copyright (C) 2020 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#ifndef _LINUX_FSINFO_H
#define _LINUX_FSINFO_H

#ifdef CONFIG_FSINFO

#include <uapi/linux/fsinfo.h>

struct path;

#define FSINFO_NORMAL_ATTR_MAX_SIZE 4096

struct fsinfo_context {
	__u32		flags;		/* [in] FSINFO_FLAGS_* */
	__u32		requested_attr;	/* [in] What is being asking for */
	__u32		Nth;		/* [in] Instance of it (some may have multiple) */
	__u32		Mth;		/* [in] Subinstance */
	bool		want_size_only;	/* [in] Just want to know the size, not the data */
	bool		clear_tail;	/* [out] T if tail of buffer should be cleared */
	unsigned int	skip;		/* [out] Number of bytes to skip in buffer */
	unsigned int	usage;		/* [tmp] Amount of buffer used (if large) */
	unsigned int	buf_size;	/* [tmp] Size of ->buffer[] */
	void		*buffer;	/* [out] The reply buffer */
};

/*
 * A filesystem information attribute definition.
 */
struct fsinfo_attribute {
	unsigned int		attr_id;	/* The ID of the attribute */
	enum fsinfo_value_type	type:8;		/* The type of the attribute's value(s) */
	unsigned int		flags:8;
	unsigned int		size:16;	/* - Value size (FSINFO_STRUCT/LIST) */
	int (*get)(struct path *path, struct fsinfo_context *params);
};

#define __FSINFO(A, T, S, G, F) \
	{ .attr_id = A, .type = T, .flags = F, .size = S, .get = G }

#define _FSINFO(A, T, S, G)	__FSINFO(A, T, S, G, 0)
#define _FSINFO_N(A, T, S, G)	__FSINFO(A, T, S, G, FSINFO_FLAGS_N)
#define _FSINFO_NM(A, T, S, G)	__FSINFO(A, T, S, G, FSINFO_FLAGS_NM)

#define _FSINFO_VSTRUCT(A,S,G)	  _FSINFO   (A, FSINFO_TYPE_VSTRUCT, sizeof(S), G)
#define _FSINFO_VSTRUCT_N(A,S,G)  _FSINFO_N (A, FSINFO_TYPE_VSTRUCT, sizeof(S), G)
#define _FSINFO_VSTRUCT_NM(A,S,G) _FSINFO_NM(A, FSINFO_TYPE_VSTRUCT, sizeof(S), G)

#define FSINFO_VSTRUCT(A,G)	_FSINFO_VSTRUCT   (A, A##__STRUCT, G)
#define FSINFO_VSTRUCT_N(A,G)	_FSINFO_VSTRUCT_N (A, A##__STRUCT, G)
#define FSINFO_VSTRUCT_NM(A,G)	_FSINFO_VSTRUCT_NM(A, A##__STRUCT, G)
#define FSINFO_STRING(A,G)	_FSINFO   (A, FSINFO_TYPE_STRING, 0, G)
#define FSINFO_STRING_N(A,G)	_FSINFO_N (A, FSINFO_TYPE_STRING, 0, G)
#define FSINFO_STRING_NM(A,G)	_FSINFO_NM(A, FSINFO_TYPE_STRING, 0, G)
#define FSINFO_OPAQUE(A,G)	_FSINFO   (A, FSINFO_TYPE_OPAQUE, 0, G)
#define FSINFO_LIST(A,G)	_FSINFO   (A, FSINFO_TYPE_LIST, sizeof(A##__STRUCT), G)
#define FSINFO_LIST_N(A,G)	_FSINFO_N (A, FSINFO_TYPE_LIST, sizeof(A##__STRUCT), G)

extern int fsinfo_opaque(const void *, struct fsinfo_context *, unsigned int);
extern int fsinfo_string(const char *, struct fsinfo_context *);
extern int fsinfo_generic_timestamp_info(struct path *, struct fsinfo_context *);
extern int fsinfo_generic_supports(struct path *, struct fsinfo_context *);
extern int fsinfo_generic_limits(struct path *, struct fsinfo_context *);
extern int fsinfo_get_attribute(struct path *, struct fsinfo_context *,
				const struct fsinfo_attribute *);

#endif /* CONFIG_FSINFO */

#endif /* _LINUX_FSINFO_H */
