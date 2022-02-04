// SPDX-License-Identifier: GPL-2.0
/*
 * Example of task isolation prctl interface with a loop:
 *
 *	do {
 *		enable quiescing of kernel activities
 *		perform computation
 *		disable quiescing of kernel activities
 *		write computation results to disk
 *	} while (condition);
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
	int ret, fd, write_loops;
	void *buf = malloc(4096);
	unsigned long mask;

	fd = open("/tmp/comp_output.data", O_RDWR|O_CREAT);
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

	ret = task_isol_setup(0);
	if (ret == -1)
		return EXIT_FAILURE;

	mask = ret;

	write_loops = 0;
	do {
#define NR_LOOPS 999999999
#define NR_PRINT 100000000
		/* enable quiescing on system call return */
		ret = task_isol_activate_set(mask);
		if (ret)
			return EXIT_FAILURE;

		/* busy loop */
		while (ret < NR_LOOPS)  {
			memset(buf, 0xf, 4096);
			ret = ret+1;
			if (!(ret % NR_PRINT))
				printf("wloop=%d loops=%d of %d\n", write_loops,
					ret, NR_LOOPS);
		}
		/* disable quiescing on system call return */
		ret = task_isol_activate_set(mask & ~ISOL_F_QUIESCE);
		if (ret)
			return EXIT_FAILURE;

		/*
		 * write computed data to disk, this would be
		 * multiple writes on a real application, so
		 * disabling quiescing is advantageous
		 */
		ret = write(fd, buf, 4096);
		if (ret == -1) {
			perror("write");
			return EXIT_FAILURE;
		}

		write_loops += 1;
	} while (write_loops < 5);


	return EXIT_SUCCESS;
}

