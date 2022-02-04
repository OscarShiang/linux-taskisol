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
#include <errno.h>
#include "task_isol.h"

#ifdef PR_ISOL_FEAT_GET
int task_isol_setup(int oneshot)
{
	int ret;
	int errnosv;
	unsigned long long fmask;
	struct task_isol_quiesce_extensions qext;
	struct task_isol_quiesce_control qctrl;

	/* Retrieve supported task isolation features */
	ret = prctl(PR_ISOL_FEAT_GET, 0, &fmask, 0, 0);
	if (ret == -1) {
		perror("prctl PR_ISOL_FEAT");
		return ret;
	}
	printf("supported features bitmask: 0x%llx\n", fmask);

	/* Retrieve supported ISOL_F_QUIESCE bits */
	ret = prctl(PR_ISOL_FEAT_GET, ISOL_F_QUIESCE, &qext, 0, 0);
	if (ret == -1) {
		perror("prctl PR_ISOL_FEAT (ISOL_F_QUIESCE)");
		return ret;
	}
	printf("supported ISOL_F_QUIESCE bits: 0x%llx\n",
		qext.supported_quiesce_bits);

	fmask = 0;
	ret = prctl(PR_ISOL_CFG_GET, I_CFG_FEAT, 0, &fmask, 0);
	errnosv = errno;
	if (ret != -1 && fmask != 0) {
		printf("Task isolation parameters already configured!\n");
		return ret;
	}
	if (ret == -1 && errnosv != ENODATA) {
		perror("prctl PR_ISOL_GET");
		return ret;
	}
	memset(&qctrl, 0, sizeof(struct task_isol_quiesce_control));
	qctrl.quiesce_mask = ISOL_F_QUIESCE_VMSTATS;
	if (oneshot)
		qctrl.quiesce_oneshot_mask = ISOL_F_QUIESCE_VMSTATS;

	ret = prctl(PR_ISOL_CFG_SET, I_CFG_FEAT, ISOL_F_QUIESCE,
		    QUIESCE_CONTROL, &qctrl);
	if (ret == -1) {
		perror("prctl PR_ISOL_CFG_SET");
		return ret;
	}
	return ISOL_F_QUIESCE;
}

int task_isol_activate_set(unsigned long long mask)
{
	int ret;

	ret = prctl(PR_ISOL_ACTIVATE_SET, &mask, 0, 0, 0);
	if (ret == -1) {
		perror("prctl PR_ISOL_ACTIVATE_SET");
		return -1;
	}

	return 0;
}

#else

int task_isol_setup(void)
{
	return 0;
}

int task_isol_activate_set(unsigned long long mask)
{
	return 0;
}
#endif


