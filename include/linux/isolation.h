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

#include <stdarg.h>
#include <linux/errno.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/irqflags.h>
#include <linux/prctl.h>
#include <linux/types.h>

#ifdef CONFIG_TASK_ISOLATION

/*
 * Logging
 *
 * This is the implementation of isolation-related messages with
 * regular kernel logging. It is intended to be human-readable and
 * should help with debugging and development of task isolation in
 * kernel and applications that use task isolation.
 *
 * In the future this mechanism may become optional or will be
 * removed, however informations in those messages will be still
 * valuable for applications. Therefore it may be replaced or
 * supplemented by event logging interface, possibly more structured
 * one, accessible to applications.
 */
int task_isolation_message(int cpu, int level, bool supp, const char *fmt, ...);

#define pr_task_isol_emerg(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_EMERG, false, fmt, ##__VA_ARGS__)
#define pr_task_isol_alert(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_ALERT, false, fmt, ##__VA_ARGS__)
#define pr_task_isol_crit(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_CRIT, false, fmt, ##__VA_ARGS__)
#define pr_task_isol_err(cpu, fmt, ...)				\
	task_isolation_message(cpu, LOGLEVEL_ERR, false, fmt, ##__VA_ARGS__)
#define pr_task_isol_warn(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_WARNING, false, fmt, ##__VA_ARGS__)
#define pr_task_isol_notice(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_NOTICE, false, fmt, ##__VA_ARGS__)
#define pr_task_isol_info(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_INFO, false, fmt, ##__VA_ARGS__)
#define pr_task_isol_debug(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_DEBUG, false, fmt, ##__VA_ARGS__)

#define pr_task_isol_emerg_supp(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_EMERG, true, fmt, ##__VA_ARGS__)
#define pr_task_isol_alert_supp(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_ALERT, true, fmt, ##__VA_ARGS__)
#define pr_task_isol_crit_supp(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_CRIT, true, fmt, ##__VA_ARGS__)
#define pr_task_isol_err_supp(cpu, fmt, ...)				\
	task_isolation_message(cpu, LOGLEVEL_ERR, true, fmt, ##__VA_ARGS__)
#define pr_task_isol_warn_supp(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_WARNING, true, fmt, ##__VA_ARGS__)
#define pr_task_isol_notice_supp(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_NOTICE, true, fmt, ##__VA_ARGS__)
#define pr_task_isol_info_supp(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_INFO, true, fmt, ##__VA_ARGS__)
#define pr_task_isol_debug_supp(cpu, fmt, ...)			\
	task_isolation_message(cpu, LOGLEVEL_DEBUG, true, fmt, ##__VA_ARGS__)

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
