# SPDX-License-Identifier: GPL-2.0-only
# List of programs to build

hostprogs := \
	test-fsinfo \
	test-fsmount \
	test-mntinfo \
	test-statx

always-y := $(hostprogs)

HOSTCFLAGS_test-fsinfo.o += -I$(objtree)/usr/include
HOSTCFLAGS_test-fsmount.o += -I$(objtree)/usr/include
HOSTCFLAGS_test-mntinfo.o += -I$(objtree)/usr/include
HOSTCFLAGS_test-statx.o += -I$(objtree)/usr/include
