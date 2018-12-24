/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CMDLINE_H
#define _LINUX_CMDLINE_H

/*
 *
 * Copyright (C) 2015. Cisco Systems, Inc.
 *
 * Generic Append/Prepend cmdline support.
 */

#if defined(CONFIG_GENERIC_CMDLINE) && defined(CONFIG_CMDLINE_BOOL)

#ifndef CONFIG_CMDLINE_OVERRIDE
/*
 * This function will append or prepend a builtin command line to the command
 * line provided by the bootloader. Kconfig options can be used to alter
 * the behavior of this builtin command line.
 * @dest: The destination of the final appended/prepended string
 * @src: The starting string or NULL if there isn't one.
 * @tmp: temporary space used for prepending
 * @length: the maximum length of the strings above.
 */
static inline void
_cmdline_add_builtin(char *dest, char *src, char *tmp, unsigned long length)
{
	if (src != dest && src != NULL) {
		strlcpy(dest, " ", length);
		strlcat(dest, src, length);
	}

	strlcat(dest, " ", length);

	if (sizeof(CONFIG_CMDLINE_APPEND) > 1)
		strlcat(dest, CONFIG_CMDLINE_APPEND, length);

	if (sizeof(CONFIG_CMDLINE_PREPEND) > 1) {
		strlcpy(tmp, CONFIG_CMDLINE_PREPEND, length);
		strlcat(tmp, " ", length);
		strlcat(tmp, dest, length);
		strlcpy(dest, tmp, length);
	}
}

#define cmdline_add_builtin_section(dest, src, length, section) 	    \
{									    \
	if (sizeof(CONFIG_CMDLINE_PREPEND) > 1) {			    \
		static char cmdline_tmp_space[length] section;	            \
		_cmdline_add_builtin(dest, src, cmdline_tmp_space, length); \
	} else {							    \
		_cmdline_add_builtin(dest, src, NULL, length);		    \
	}								    \
}
#else
#define cmdline_add_builtin_section(dest, src, length, section) 	   \
{									   \
	strlcpy(dest, CONFIG_CMDLINE_PREPEND " " CONFIG_CMDLINE_APPEND,    \
		length);						   \
}
#endif /* !CONFIG_CMDLINE_OVERRIDE */

#else
#define cmdline_add_builtin_section(dest, src, length, section) {          \
	if (src != NULL)						   \
		strlcpy(dest, src, length);				   \
}
#endif /* CONFIG_GENERIC_CMDLINE */

#define cmdline_add_builtin(dest, src, length) \
	cmdline_add_builtin_section(dest, src, length, __initdata)

#endif /* _LINUX_CMDLINE_H */
