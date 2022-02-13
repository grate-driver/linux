// SPDX-License-Identifier: GPL-2.0
/*
 * Naive system call dropper built on seccomp_filter.
 *
 * Copyright (c) 2012 The Chromium OS Authors <chromium-os-dev@chromium.org>
 * Author: Will Drewry <wad@chromium.org>
 *
 * The code may be used by anyone for any purpose,
 * and can serve as a starting point for developing
 * applications using prctl(PR_SET_SECCOMP, 2, ...).
 *
 * When run, returns the specified errno for the specified
 * system call number against the given architecture.
 *
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/ptrace.h>
#include <linux/seccomp.h>
#include <linux/unistd.h>

static unsigned int get_syscall_arch(void)
{
	struct ptrace_syscall_info info = { };
	siginfo_t siginfo = { };
	unsigned int arch = -1;
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) {
			perror("PTRACE_TRACEME");
			_exit(1);
		}
		if (raise(SIGSTOP) != 0) {
			perror("raise");
			_exit(1);
		}
		_exit(0);
	}
	if (ptrace(PTRACE_ATTACH, pid, 0, 0) != 0)
		goto reap;
	if (waitid(P_PID, pid, &siginfo, WEXITED | WSTOPPED | WCONTINUED) != 0)
		goto reap;
	if (siginfo.si_code != CLD_STOPPED &&
	    siginfo.si_code != CLD_TRAPPED)
		goto reap;
	if (ptrace(PTRACE_GET_SYSCALL_INFO, pid, sizeof(info), &info)
	    < offsetof(typeof(info), arch) + sizeof(info.arch))
		goto reap;
	arch = info.arch;
	ptrace(PTRACE_DETACH, pid, 0, 0);
reap:
	kill(pid, SIGKILL);
	if (waitpid(pid, NULL, 0) != pid)
		perror("waitpid");
	return arch;
}

static int install_filter(int arch, int nr, int error)
{
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD+BPF_W+BPF_ABS,
			 (offsetof(struct seccomp_data, arch))),
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, arch, 0, 3),
		BPF_STMT(BPF_LD+BPF_W+BPF_ABS,
			 (offsetof(struct seccomp_data, nr))),
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, nr, 0, 1),
		BPF_STMT(BPF_RET+BPF_K,
			 SECCOMP_RET_ERRNO|(error & SECCOMP_RET_DATA)),
		BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = (unsigned short)(sizeof(filter)/sizeof(filter[0])),
		.filter = filter,
	};
	if (error == -1) {
		struct sock_filter kill = BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_KILL);
		filter[4] = kill;
	}
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		perror("prctl(NO_NEW_PRIVS)");
		return 1;
	}
	if (prctl(PR_SET_SECCOMP, 2, &prog)) {
		perror("prctl(PR_SET_SECCOMP)");
		return 1;
	}
	return 0;
}

#define hint(arch)	do {				\
	fprintf(stderr, "\t  " #arch ":\t0x%X", arch);	\
	if (arch == native) {				\
		fprintf(stderr, " (native)");		\
		seen = 1;				\
	}						\
	fprintf(stderr, "\n");				\
} while (0)

int main(int argc, char **argv)
{
	if (argc < 5) {
		int seen = 0;
		unsigned int native;

		fprintf(stderr, "Usage: "
			"dropper <arch> <syscall_nr> [-1|<errno>] <prog> [<args>]\n"
			"arch: linux/audit.h AUDIT_ARCH_* for filter\n\tHint:\n");
		native = get_syscall_arch();
		hint(AUDIT_ARCH_X86_64);
		hint(AUDIT_ARCH_I386);
		hint(AUDIT_ARCH_AARCH64);
		hint(AUDIT_ARCH_ARM);
		if (!seen)
			fprintf(stderr, "\t  native:\t\t0x%X\n", native);
		fprintf(stderr,
			"errno: errno to set or -1 to perform SECCOMP_RET_KILL\n");
		return 1;
	}
	if (install_filter(strtol(argv[1], NULL, 0), strtol(argv[2], NULL, 0),
			   strtol(argv[3], NULL, 0)))
		return 1;
	execv(argv[4], &argv[4]);
	printf("Failed to execv\n");
	return 255;
}
