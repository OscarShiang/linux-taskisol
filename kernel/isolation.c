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

/*
 * Description of the last two tasks that ran isolated on a given CPU.
 * This is intended only for reporting isolation breaking. We don't
 * want any references to actual task while accessing this from CPU
 * that caused isolation breaking -- we know nothing about timing and
 * don't want to use locking or RCU.
 */
struct isol_task_desc {
	atomic_t curr_index;
	atomic_t curr_index_wr;
	bool	warned[2];
	pid_t	pid[2];
	pid_t	tgid[2];
	char	comm[2][TASK_COMM_LEN];
};
static DEFINE_PER_CPU(struct isol_task_desc, isol_task_descs);

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
 * Record name, pid and group pid of the task entering isolation on
 * the current CPU. This information will be used to report isolation
 * breaking.
 */
static void record_curr_isolated_task(void)
{
	int ind;
	int cpu = smp_processor_id();
	struct isol_task_desc *desc = &per_cpu(isol_task_descs, cpu);
	struct task_struct *task = current;

	/* Finish everything before recording current task */
	smp_mb();
	ind = atomic_inc_return(&desc->curr_index_wr) & 1;
	desc->comm[ind][sizeof(task->comm) - 1] = '\0';
	memcpy(desc->comm[ind], task->comm, sizeof(task->comm) - 1);
	desc->pid[ind] = task->pid;
	desc->tgid[ind] = task->tgid;
	desc->warned[ind] = false;
	/* Write everything, to be seen by other CPUs */
	smp_mb();
	atomic_inc(&desc->curr_index);
	/* Everyone will see the new record from this point */
	smp_mb();
}

/*
 * Print message prefixed with the description of the current (or
 * last) isolated task on a given CPU. Intended for isolation breaking
 * messages that include target task for the user's convenience.
 *
 * Messages produced with this function may have obsolete task
 * information if isolated tasks managed to exit, start and enter
 * isolation multiple times, or multiple tasks tried to enter
 * isolation on the same CPU at once. For those unusual cases it would
 * contain a valid description of the cause for isolation breaking and
 * target CPU number, just not the correct description of which task
 * ended up losing isolation.
 *
 * A similar mechanism may be provided in the future to record events
 * in a manner readable by applications.
 */
int task_isolation_message(int cpu, int level, bool supp, const char *fmt, ...)
{
	struct isol_task_desc *desc;
	struct task_struct *task;
	va_list args;
	char buf_prefix[TASK_COMM_LEN + 20 + 3 * 20];
	char buf[200];
	int curr_cpu, ind_counter, ind_counter_old, ind;

	curr_cpu = get_cpu();
	/* Barrier to synchronize with recording isolated task information */
	smp_rmb();
	desc = &per_cpu(isol_task_descs, cpu);
	ind_counter = atomic_read(&desc->curr_index);

	if (curr_cpu == cpu) {
		/*
		 * Message is for the current CPU so current
		 * task_struct should be used instead of cached
		 * information.
		 *
		 * Like in other diagnostic messages, if issued from
		 * interrupt context, current will be the interrupted
		 * task. Unlike other diagnostic messages, this is
		 * always relevant because the message is about
		 * interrupting a task.
		 */
		ind = ind_counter & 1;
		if (supp && desc->warned[ind]) {
			/*
			 * If supp is true, skip the message if the
			 * same task was mentioned in the message
			 * originated on remote CPU, and it did not
			 * re-enter isolated state since then (warned
			 * is true). Only local messages following
			 * remote messages, likely about the same
			 * isolation breaking event, are skipped to
			 * avoid duplication. If remote cause is
			 * immediately followed by a local one before
			 * isolation is broken, local cause is skipped
			 * from messages.
			 */
			put_cpu();
			return 0;
		}
		task = current;
		snprintf(buf_prefix, sizeof(buf_prefix),
			 "isolation %s/%d/%d (cpu %d)",
			 task->comm, task->tgid, task->pid, cpu);
		put_cpu();
	} else {
		/*
		 * Message is for remote CPU, use cached information.
		 */
		put_cpu();
		/*
		 * Make sure, index remained unchanged while data was
		 * copied. If it changed, data that was copied may be
		 * inconsistent because two updates in a sequence could
		 * overwrite the data while it was being read.
		 */
		do {
			/* Make sure we are reading up to date values */
			smp_mb();
			ind = ind_counter & 1;
			snprintf(buf_prefix, sizeof(buf_prefix),
				 "isolation %s/%d/%d (cpu %d)",
				 desc->comm[ind], desc->tgid[ind],
				 desc->pid[ind], cpu);
			desc->warned[ind] = true;
			ind_counter_old = ind_counter;
			/* Record the warned flag, then re-read descriptor */
			smp_mb();
			ind_counter = atomic_read(&desc->curr_index);
			/*
			 * If the counter changed, something was updated, so
			 * repeat everything to get the current data
			 */
		} while (ind_counter != ind_counter_old);
	}

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	switch (level) {
	case LOGLEVEL_EMERG:
		pr_emerg("%s: %s", buf_prefix, buf);
		break;
	case LOGLEVEL_ALERT:
		pr_alert("%s: %s", buf_prefix, buf);
		break;
	case LOGLEVEL_CRIT:
		pr_crit("%s: %s", buf_prefix, buf);
		break;
	case LOGLEVEL_ERR:
		pr_err("%s: %s", buf_prefix, buf);
		break;
	case LOGLEVEL_WARNING:
		pr_warn("%s: %s", buf_prefix, buf);
		break;
	case LOGLEVEL_NOTICE:
		pr_notice("%s: %s", buf_prefix, buf);
		break;
	case LOGLEVEL_INFO:
		pr_info("%s: %s", buf_prefix, buf);
		break;
	case LOGLEVEL_DEBUG:
		pr_debug("%s: %s", buf_prefix, buf);
		break;
	default:
		/* No message without a valid level */
		return 0;
	}
	return 1;
}

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
