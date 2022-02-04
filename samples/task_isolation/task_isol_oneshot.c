// SPDX-License-Identifier: GPL-2.0
/*
 * Example of task isolation prctl interface using
 * oneshot mode for quiescing.
 *
 *
 *      enable oneshot quiescing of kernel activities
 *	do {
 *		process data (no system calls)
 *		if (event) {
 *			process event with syscalls
 *			enable oneshot quiescing of kernel activities
 *		}
 *	} while (!exit_condition);
 *
 */
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <linux/prctl.h>
#include "task_isol.h"

int main(void)
{
	int ret, fd, cnt;
	void *buf = malloc(4096);
	unsigned long mask;

	fd = open("/dev/zero", O_RDONLY);
	if (fd == -1) {
		perror("open");
		return EXIT_FAILURE;
	}

	memset(buf, 1, 4096);
	ret = mlock(buf, 4096);
	if (ret) {
		perror("mlock");
		return EXIT_FAILURE;
	}

	ret = task_isol_setup(1);
	if (ret == -1)
		return EXIT_FAILURE;

	mask = ret;

#define NR_LOOPS 999999999
#define NR_PRINT 100000000

	/* enable quiescing on system call return, oneshot */
	ret = task_isol_activate_set(mask);
	if (ret)
		return EXIT_FAILURE;
	/* busy loop */
	cnt = 0;
	while (cnt < NR_LOOPS)  {
		memset(buf, 0xf, 4096);
		cnt = cnt+1;
		if (!(cnt % NR_PRINT)) {
			int i, r;

			/* this could be considered handling an external
			 * event: with one-shot mode, system calls
			 * after prctl(PR_SET_ACTIVATE) will not incur
			 * the penalty of quiescing
			 */
			printf("loops=%d of %d\n", cnt, NR_LOOPS);
			for (i = 0; i < 100; i++) {
				r = read(fd, buf, 4096);
				if (r == -1) {
					perror("read");
					return EXIT_FAILURE;
				}
			}

			ret = munlock(buf, 4096);
			if (ret) {
				perror("munlock");
				return EXIT_FAILURE;
			}

			ret = mlock(buf, 4096);
			if (ret) {
				perror("mlock");
				return EXIT_FAILURE;
			}

			/* enable quiescing on system call return */
			ret = task_isol_activate_set(mask);
			if (ret)
				return EXIT_FAILURE;
		}
	}

	return EXIT_SUCCESS;
}

