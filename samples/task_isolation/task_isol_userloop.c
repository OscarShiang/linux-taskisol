// SPDX-License-Identifier: GPL-2.0
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
	int ret;
	void *buf = malloc(4096);
	unsigned long mask;

	memset(buf, 1, 4096);
	ret = mlock(buf, 4096);
	if (ret) {
		perror("mlock");
		return EXIT_FAILURE;
	}

	ret = task_isol_setup(0);
	if (ret == -1)
		return EXIT_FAILURE;

	mask = ret;
	/* enable quiescing on system call return, oneshot */
	ret = task_isol_activate_set(mask);
	if (ret)
		return EXIT_FAILURE;

#define NR_LOOPS 999999999
#define NR_PRINT 100000000
	/* busy loop */
	while (ret < NR_LOOPS)  {
		memset(buf, 0, 4096);
		ret = ret+1;
		if (!(ret % NR_PRINT))
			printf("loops=%d of %d\n", ret, NR_LOOPS);
	}


	ret = task_isol_activate_set(mask & ~ISOL_F_QUIESCE);
	if (ret)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

