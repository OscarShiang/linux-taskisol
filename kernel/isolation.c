// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Implementation of task isolation.
 *
 * Authors:
 *   Chris Metcalf <cmetcalf@mellanox.com>
 *   Alex Belits <abelits@marvell.com>
 *   Yuri Norov <ynorov@marvell.com>
 */

#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/sched.h>
#include <linux/isolation.h>
#include <linux/syscalls.h>
#include <linux/smp.h>
#include <linux/tick.h>
#include <asm/unistd.h>
#include <asm/syscall.h>
#include <linux/hrtimer.h>

/*
 * These values are stored in task_isolation_state.
 * Note that STATE_NORMAL + TIF_TASK_ISOLATION means we are still
 * returning from sys_prctl() to userspace.
 */
enum {
	STATE_NORMAL = 0,	/* Not isolated */
	STATE_ISOLATED = 1	/* In userspace, isolated */
};

/*
 * Low-level isolation flags.
 * Those flags are used by low-level isolation set/clear/check routines.
 * Those flags should be set last before return to userspace and cleared
 * first upon kernel entry, and synchronized to allow isolation breaking
 * detection before touching potentially unsynchronized parts of kernel.
 * Isolated task does not receive synchronization events of any kind, so
 * at the time of the first entry into kernel it might not be ready to
 * run most of the kernel code. However to perform synchronization
 * properly, kernel entry code should also enable synchronization events
 * at the same time. This presents a problem because more kernel code
 * should run to determine the cause of isolation breaking, signals may
 * have to be generated, etc. So some flag clearing and synchronization
 * should happen in "low-level" entry code but processing of isolation
 * breaking should happen in "high-level" code. Low-level isolation flags
 * should be set in that low-level code, possibly long before the cause
 * of isolation breaking is known. Symmetrically, entering isolation
 * should disable synchronization events before returning to userspace
 * but after all potentially volatile code is finished.
 */
DEFINE_PER_CPU(unsigned long, ll_isol_flags);

cpumask_var_t task_isolation_map;

/* We can run on cpus that are isolated from the scheduler and are nohz_full. */
static int __init task_isolation_init(void)
{
	alloc_bootmem_cpumask_var(&task_isolation_cleanup_map);
	if (alloc_cpumask_var(&task_isolation_map, GFP_KERNEL))
		/*
		 * At this point task isolation should match
		 * nohz_full. This may change in the future.
		 */
		cpumask_copy(task_isolation_map, tick_nohz_full_mask);
	return 0;
}
core_initcall(task_isolation_init)

/*
 * Set the flags word but don't try to actually start task isolation yet.
 * We will start it when entering user space.
 */
int task_isolation_request(unsigned int flags)
{
	struct task_struct *task = current;

	/*
	 * The task isolation flags should always be cleared just by
	 * virtue of having entered the kernel.
	 */
	WARN_ON_ONCE(test_tsk_thread_flag(task, TIF_TASK_ISOLATION));
	WARN_ON_ONCE(task->task_isolation_flags != 0);
	WARN_ON_ONCE(task->task_isolation_state != STATE_NORMAL);

	task->task_isolation_flags = flags;

	if (!(task->task_isolation_flags & PR_TASK_ISOLATION_ENABLE))
		return 0;

	/* We are trying to enable task isolation. */
	set_tsk_thread_flag(task, TIF_TASK_ISOLATION);

	/*
	 * Shut down the vmstat worker so we're not interrupted later.
	 * We have to try to do this here (with interrupts enabled) since
	 * we are canceling delayed work and will call flush_work()
	 * (which enables interrupts) and possibly schedule().
	 */
	quiet_vmstat_sync();

	/*
	 * We return 0 here, however the error value may be still
	 * produced before return to userspace.
	 */
	return 0;
}

/*
 * Set CPUs currently running isolated tasks in CPU mask.
 */
void task_isolation_cpumask(struct cpumask *mask)
{
	int cpu;

	if (task_isolation_map == NULL)
		return;

	/* Barrier to synchronize with writing task isolation flags */
	smp_rmb();
	for_each_cpu(cpu, task_isolation_map)
		if (task_isolation_on_cpu(cpu))
			cpumask_set_cpu(cpu, mask);
}

/*
 * Clear CPUs currently running isolated tasks in CPU mask.
 */
void task_isolation_clear_cpumask(struct cpumask *mask)
{
	int cpu;

	if (task_isolation_map == NULL)
		return;

	/* Barrier to synchronize with writing task isolation flags */
	smp_rmb();
	for_each_cpu(cpu, task_isolation_map)
		if (task_isolation_on_cpu(cpu))
			cpumask_clear_cpu(cpu, mask);
}
