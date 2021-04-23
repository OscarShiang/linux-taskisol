/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Task isolation support
 *
 * Authors:
 *   Chris Metcalf <cmetcalf@mellanox.com>
 *   Alex Belits <abelits@marvell.com>
 *   Yuri Norov <ynorov@marvell.com>
 */
#ifndef _LINUX_ISOLATION_H
#define _LINUX_ISOLATION_H

#include <linux/cpumask.h>
#include <linux/percpu.h>

#ifdef CONFIG_TASK_ISOLATION

#define BIT_LL_TASK_ISOLATION		(0)
#define BIT_LL_TASK_ISOLATION_BROKEN	(1)
#define BIT_LL_TASK_ISOLATION_REQUEST	(2)
#define FLAG_LL_TASK_ISOLATION		(1 << BIT_LL_TASK_ISOLATION)
#define FLAG_LL_TASK_ISOLATION_BROKEN	(1 << BIT_LL_TASK_ISOLATION_BROKEN)
#define FLAG_LL_TASK_ISOLATION_REQUEST	(1 << BIT_LL_TASK_ISOLATION_REQUEST)

DECLARE_PER_CPU(unsigned long, ll_isol_flags);
extern cpumask_var_t task_isolation_map;

/**
 * task_isolation_request() - prctl hook to request task isolation
 * @flags:	Flags from <uapi/linux/prctl.h> PR_TASK_ISOLATION_xxx.
 *
 * This is called from the generic prctl() code for PR_TASK_ISOLATION.
 *
 * Return: Returns 0 when task isolation enabled, otherwise a negative
 * errno.
 */
extern int task_isolation_request(unsigned int flags);

/**
 * is_isolation_cpu() - check if CPU is intended for running isolated tasks.
 * @cpu:	CPU to check.
 */
static inline bool is_isolation_cpu(int cpu)
{
	return task_isolation_map != NULL &&
		cpumask_test_cpu(cpu, task_isolation_map);
}

/**
 * task_isolation_on_cpu() - check if the cpu is running isolated task
 * @cpu:	CPU to check.
 */
static inline int task_isolation_on_cpu(int cpu)
{
	return test_bit(BIT_LL_TASK_ISOLATION, &per_cpu(ll_isol_flags, cpu));
}

/**
 * task_isolation_cpumask() - set CPUs currently running isolated tasks
 * @mask:	Mask to modify.
 */
extern void task_isolation_cpumask(struct cpumask *mask);

/**
 * task_isolation_clear_cpumask() - clear CPUs currently running isolated tasks
 * @mask:      Mask to modify.
 */
extern void task_isolation_clear_cpumask(struct cpumask *mask);

#else /* !CONFIG_TASK_ISOLATION */
static inline int task_isolation_request(unsigned int flags) { return -EINVAL; }
static inline bool is_isolation_cpu(int cpu) { return 0; }
static inline int task_isolation_on_cpu(int cpu) { return 0; }
static inline void task_isolation_cpumask(struct cpumask *mask) { }
static inline void task_isolation_clear_cpumask(struct cpumask *mask) { }
#endif

#endif /* _LINUX_ISOLATION_H */
