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

struct task_struct;

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
 * task_isolation_kernel_enter() - clear low-level task isolation flag
 *
 * This should be called immediately after entering kernel. It must
 * be inline, and suitable for running after leaving isolated
 * userspace in a "stale" state when synchronization is required
 * before the CPU can safely enter the rest of the kernel.
 */
static __always_inline void task_isolation_kernel_enter(void)
{
	unsigned long flags;

	/*
	 * This function runs on a CPU that ran isolated task.
	 *
	 * We don't want this CPU running code from the rest of kernel
	 * until other CPUs know that it is no longer isolated.  When
	 * CPU is running isolated task until this point anything that
	 * causes an interrupt on this CPU must end up calling this
	 * before touching the rest of kernel. That is, this function
	 * or fast_task_isolation_cpu_cleanup() or stop_isolation()
	 * calling it. If any interrupt, including scheduling timer,
	 * arrives, it will still end up here early after entering
	 * kernel.  From this point interrupts are disabled until all
	 * CPUs will see that this CPU is no longer running isolated
	 * task.
	 *
	 * See also fast_task_isolation_cpu_cleanup().
	 */
	if ((this_cpu_read(ll_isol_flags) & FLAG_LL_TASK_ISOLATION) == 0)
		return;

	raw_local_irq_save(flags);

	/*
	 * There is a possibility that we are entering kernel on a CPU
	 * where ll_isol_flags still has FLAG_LL_TASK_ISOLATION set
	 * from the previously running task, however a new task is
	 * scheduled to run there. If a new task is running here, and
	 * it is not isolated, synchronization should be performed
	 * before it will run there, however there is no actual
	 * isolation breaking happening.
	 *
	 * If this is an isolated task, change low-level flags to
	 * indicate broken isolation, otherwise erase them.
	 */
	if (current->task_isolation_state != STATE_NORMAL)
		this_cpu_write(ll_isol_flags, FLAG_LL_TASK_ISOLATION_BROKEN);
	else
		this_cpu_write(ll_isol_flags, 0);

	/*
	 * If something happened that requires a barrier that would
	 * otherwise be called from remote CPUs by CPU kick procedure,
	 * this barrier runs instead of it. After this barrier, CPU
	 * kick procedure would see the updated ll_isol_flags, so it
	 * will run its own IPI to trigger a barrier.
	 */
	smp_mb();
	/*
	 * Synchronize instructions -- this CPU was not kicked while
	 * in isolated mode, so it might require synchronization.
	 * There might be an IPI if kick procedure happened and
	 * ll_isol_flags was already updated while it assembled a CPU
	 * mask. However if this did not happen, synchronize everything
	 * here.
	 */
	instr_sync();
	raw_local_irq_restore(flags);
}

/**
 * task_isolation_exit_to_user_mode() - set low-level task isolation flag
 * if task isolation is requested
 *
 * This should be called immediately before exiting kernel. It must
 * be inline, and the state of CPI may become "stale" between setting
 * the flag and returning to the userspace.
 */
static __always_inline void task_isolation_exit_to_user_mode(void)
{
	unsigned long flags;

	/* Check if this task is entering isolation */
	if ((this_cpu_read(ll_isol_flags) & FLAG_LL_TASK_ISOLATION_REQUEST)
	    == 0)
		return;
	raw_local_irq_save(flags);

	/* Set low-level flags */
	this_cpu_write(ll_isol_flags, FLAG_LL_TASK_ISOLATION);
	/*
	 * After this barrier the rest of the system stops using IPIs
	 * to synchronize this CPU state. Since only exit to userspace
	 * follows, this is safe. Synchronization will happen again in
	 * task_isolation_enter() when this CPU will enter kernel.
	 */
	smp_mb();
	/*
	 * From this point this is recognized as isolated by
	 * other CPUs
	 */
	raw_local_irq_restore(flags);
}

extern void task_isolation_cpu_cleanup(void);

/**
 * task_isolation_start() - attempt to actually start task isolation
 *
 * This function should be invoked as the last thing prior to returning to
 * user space if TIF_TASK_ISOLATION is set in the thread_info flags.  It
 * will attempt to quiesce the core and enter task-isolation mode.  If it
 * fails, it will reset the system call return value to an error code that
 * indicates the failure mode.
 */
extern void task_isolation_start(void);

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

/**
 * task_isolation_before_pending_work_check() - check for isolation breaking
 *
 * This routine is called from the code responsible for exiting to user mode,
 * before the point when thread flags are checked for pending work.
 * That function must be called if the current task is isolated, because
 * TIF_TASK_ISOLATION must trigger a call to it.
 */
void task_isolation_before_pending_work_check(void);

#else /* !CONFIG_TASK_ISOLATION */
static inline int task_isolation_request(unsigned int flags) { return -EINVAL; }
static inline void task_isolation_kernel_enter(void) {}
static inline void task_isolation_exit_to_user_mode(void) {}
static inline void task_isolation_start(void) { }
static inline bool is_isolation_cpu(int cpu) { return 0; }
static inline int task_isolation_on_cpu(int cpu) { return 0; }
static inline void task_isolation_cpumask(struct cpumask *mask) { }
static inline void task_isolation_clear_cpumask(struct cpumask *mask) { }
static inline void task_isolation_cpu_cleanup(void) { }
static inline void task_isolation_before_pending_work_check(void) { }
#endif

#endif /* _LINUX_ISOLATION_H */
