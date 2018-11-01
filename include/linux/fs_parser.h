/* Filesystem parameter description and parser
 *
 * Copyright (C) 2018 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#ifndef _LINUX_FS_PARSER_H
#define _LINUX_FS_PARSER_H

#include <linux/fs_context.h>

struct path;

struct constant_table {
	const char	*name;
	int		value;
};

#define fsconfig_key_removed	0xff	/* Parameter name is no longer valid */

/*
 * The type of parameter expected.
 */
enum fs_parameter_type {
	__fs_param_wasnt_defined,
	fs_param_is_flag,
	fs_param_is_bool,
	fs_param_is_u32,
	fs_param_is_u32_octal,
	fs_param_is_u32_hex,
	fs_param_is_s32,
	fs_param_is_u64,
	fs_param_is_enum,
	fs_param_is_string,
	fs_param_is_blob,
	fs_param_is_blockdev,
	fs_param_is_path,
	fs_param_is_fd,
	nr__fs_parameter_type,
};

/*
 * Specification of the type of value a parameter wants.
 */
struct fs_parameter_spec {
	enum fs_parameter_type	type:8;	/* The desired parameter type */
	u8			flags;
#define fs_param_v_optional	0x01	/* The value is optional */
#define fs_param_neg_with_no	0x02	/* "noxxx" is negative param */
#define fs_param_neg_with_empty	0x04	/* "xxx=" is negative param */
#define fs_param_deprecated	0x08	/* The param is deprecated */
};

struct fs_parameter_enum {
	u8		param_id;
	char		name[14];
	u8		value;
};

struct fs_parameter_description {
	const char	name[16];	/* Name for logging purposes */
	u8		nr_params;	/* Number of parameter IDs */
	u8		nr_alt_keys;	/* Number of alt_keys[] */
	u8		nr_enums;	/* Number of enum value names */
	u8		source_param;	/* Index of source parameter */
	bool		no_source;	/* Set if no source is expected */
	const char *const *keys;	/* Sorted list of key names, one per nr_params */
	const struct constant_table *alt_keys; /* Sorted list of alternate key names */
	const struct fs_parameter_spec *specs; /* List of param specifications */
	const struct fs_parameter_enum *enums; /* Enum values */
};

/*
 * Result of parse.
 */
struct fs_parse_result {
	struct fs_parameter_spec t;
	u8			key;		/* Looked up key ID */
	bool			negated;	/* T if param was "noxxx" */
	bool			has_value;	/* T if value supplied to param */
	union {
		bool		boolean;	/* For spec_bool */
		int		int_32;		/* For spec_s32/spec_enum */
		unsigned int	uint_32;	/* For spec_u32{,_octal,_hex}/spec_enum */
		u64		uint_64;	/* For spec_u64 */
	};
};

extern int fs_parse(struct fs_context *fc,
		    const struct fs_parameter_description *desc,
		    struct fs_parameter *value,
		    struct fs_parse_result *result);
extern int fs_lookup_param(struct fs_context *fc,
			   struct fs_parameter *param,
			   bool want_bdev,
			   struct path *_path);

extern int __lookup_constant(const struct constant_table tbl[], size_t tbl_size,
			     const char *name, int not_found);
#define lookup_constant(t, n, nf) __lookup_constant(t, ARRAY_SIZE(t), (n), (nf))

#ifdef CONFIG_VALIDATE_FS_PARSER
extern bool validate_constant_table(const struct constant_table *tbl, size_t tbl_size,
				    int low, int high, int special);
extern bool fs_validate_description(const struct fs_parameter_description *desc);
#else
static inline bool validate_constant_table(const struct constant_table *tbl, size_t tbl_size,
					   int low, int high, int special)
{ return true; }
static inline bool fs_validate_description(const struct fs_parameter_description *desc)
{ return true; }
#endif

#endif /* _LINUX_FS_PARSER_H */
