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

/*
 * Counter for isolation exiting procedures (from request to the start of
 * cleanup) being attempted at once on a CPU. Normally incrementing of
 * this counter is performed from the CPU that caused isolation breaking,
 * however decrementing is done from the cleanup procedure, delegated to
 * the CPU that is exiting isolation, not from the CPU that caused isolation
 * breaking.
 *
 * If incrementing this counter while starting isolation exit procedure
 * results in a value greater than 0, isolation exiting is already in
 * progress, and cleanup did not start yet. This means, counter should be
 * decremented back, and isolation exit that is already in progress, should
 * be allowed to complete. Otherwise, a new isolation exit procedure should
 * be started.
 */
DEFINE_PER_CPU(atomic_t, isol_exit_counter);

/*
 * Descriptor for isolation-breaking SMP calls
 */
DEFINE_PER_CPU(call_single_data_t, isol_break_csd);

cpumask_var_t task_isolation_map;
cpumask_var_t task_isolation_cleanup_map;
static DEFINE_SPINLOCK(task_isolation_cleanup_lock);

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
 * Perform actions that should be done immediately on exit from isolation.
 */
static void fast_task_isolation_cpu_cleanup(void *info)
{
	unsigned long flags;

	/*
	 * This function runs on a CPU that ran isolated task.
	 * It should be called either directly when isolation breaking is
	 * being processed, or using IPI from another CPU when it intends
	 * to break isolation on the given CPU.
	 *
	 * We don't want this CPU running code from the rest of kernel
	 * until other CPUs know that it is no longer isolated. Any
	 * entry into kernel will call task_isolation_kernel_enter()
	 * before calling this, so this will be already done by
	 * setting per-cpu flags and synchronizing in that function.
	 *
	 * For development purposes it makes sense to check if it was
	 * done, because there is a possibility that some entry points
	 * were left unguarded. That would be clearly a bug because it
	 * will mean that regular kernel code is running with no
	 * synchronization.
	 */
	local_irq_save(flags);
	atomic_dec(&per_cpu(isol_exit_counter, smp_processor_id()));
	/* Barrier to sync with requesting a task isolation breaking */
	smp_mb__after_atomic();
	/*
	 * At this point breaking isolation will be treated as a
	 * separate isolation-breaking event, however interrupts won't
	 * arrive until local_irq_restore()
	 */

	/*
	 * Check for the above mentioned entry without a call to
	 * task_isolation_kernel_enter()
	 */
	if ((this_cpu_read(ll_isol_flags) & FLAG_LL_TASK_ISOLATION)) {
		/*
		 * If it did happen, call the function here, to
		 * prevent further problems from running in
		 * un-synchronized state
		 */
		task_isolation_kernel_enter();
		/* Report the problem */
		pr_task_isol_emerg(smp_processor_id(),
		"Isolation breaking was not detected on kernel entry\n");
	}
	/*
	 * This task is no longer isolated (and if by any chance this
	 * is the wrong task, it's already not isolated)
	 */
	current->task_isolation_flags = 0;
	clear_tsk_thread_flag(current, TIF_TASK_ISOLATION);

	/* Run the rest of cleanup later */
	set_tsk_thread_flag(current, TIF_NOTIFY_RESUME);

	local_irq_restore(flags);
}

/* Disable task isolation for the specified task. */
static void stop_isolation(struct task_struct *p)
{
	int cpu, this_cpu;
	unsigned long flags;

	this_cpu = get_cpu();
	cpu = task_cpu(p);
	if (atomic_inc_return(&per_cpu(isol_exit_counter, cpu)) > 1) {
		/* Already exiting isolation */
		atomic_dec(&per_cpu(isol_exit_counter, cpu));
		put_cpu();
		return;
	}

	if (p == current) {
		p->task_isolation_state = STATE_NORMAL;
		fast_task_isolation_cpu_cleanup(NULL);
		task_isolation_cpu_cleanup();
		put_cpu();
	} else {
		/*
		 * Schedule "slow" cleanup. This relies on
		 * TIF_NOTIFY_RESUME being set
		 */
		spin_lock_irqsave(&task_isolation_cleanup_lock, flags);
		cpumask_set_cpu(cpu, task_isolation_cleanup_map);
		spin_unlock_irqrestore(&task_isolation_cleanup_lock, flags);
		/*
		 * Setting flags is delegated to the CPU where
		 * isolated task is running
		 * isol_exit_counter will be decremented from there as well.
		 */
		per_cpu(isol_break_csd, cpu).func =
		    fast_task_isolation_cpu_cleanup;
		per_cpu(isol_break_csd, cpu).info = NULL;
		per_cpu(isol_break_csd, cpu).flags = 0;
		smp_call_function_single_async(cpu,
					       &per_cpu(isol_break_csd, cpu));
		put_cpu();
	}
}

/*
 * This code runs with interrupts disabled just before the return to
 * userspace, after a prctl() has requested enabling task isolation.
 * We take whatever steps are needed to avoid being interrupted later:
 * drain the lru pages, stop the scheduler tick, etc. More
 * functionality may be added here later to avoid other types of
 * interrupts from other kernel subsystems. This, however, may still not
 * have the intended result, so the rest of the system takes into account
 * the possibility of receiving an interrupt and isolation breaking later.
 *
 * If we can't enable task isolation, we update the syscall return
 * value with an appropriate error.
 *
 * This, however, does not enable isolation yet, as far as low-level
 * flags are concerned. So if interrupts will be enabled, it's still
 * possible for the task to be interrupted. The call to
 * task_isolation_exit_to_user_mode() should finally enable task
 * isolation after this function set FLAG_LL_TASK_ISOLATION_REQUEST.
 */
void task_isolation_start(void)
{
	int error;
	unsigned long flags;

	/*
	 * We should only be called in STATE_NORMAL (isolation
	 * disabled), on our way out of the kernel from the prctl()
	 * that turned it on.  If we are exiting from the kernel in
	 * another state, it means we made it back into the kernel
	 * without disabling task isolation, and we should investigate
	 * how (and in any case disable task isolation at this
	 * point). We are clearly not on the path back from the
	 * prctl() so we don't touch the syscall return value.
	 */
	if (WARN_ON_ONCE(current->task_isolation_state != STATE_NORMAL)) {
		stop_isolation(current);
		/* Report the problem */
		pr_task_isol_emerg(smp_processor_id(),
		"Isolation start requested while not in the normal state\n");
		return;
	}

	/*
	 * Must be affinitized to a single core with task isolation possible.
	 * In principle this could be remotely modified between the prctl()
	 * and the return to userspace, so we have to check it here.
	 */
	if (current->nr_cpus_allowed != 1 ||
	    !is_isolation_cpu(smp_processor_id())) {
		error = -EINVAL;
		goto error;
	}

	/* If the vmstat delayed work is not canceled, we have to try again. */
	if (!vmstat_idle()) {
		error = -EAGAIN;
		goto error;
	}

	/* Try to stop the dynamic tick. */
	error = try_stop_full_tick();
	if (error)
		goto error;

	/* Drain the pagevecs to avoid unnecessary IPI flushes later. */
	lru_add_drain();

	local_irq_save(flags);

	/* Record isolated task IDs and name */
	record_curr_isolated_task();

	current->task_isolation_state = STATE_ISOLATED;
	this_cpu_write(ll_isol_flags, FLAG_LL_TASK_ISOLATION_REQUEST);
	/* Barrier to synchronize with reading of flags */
	smp_mb();
	local_irq_restore(flags);
	return;

error:
	stop_isolation(current);
	syscall_set_return_value(current, current_pt_regs(), error, 0);
}

/* Stop task isolation on the remote task and send it a signal. */
static void send_isolation_signal(struct task_struct *task)
{
	int flags = task->task_isolation_flags;
	kernel_siginfo_t info = {
		.si_signo = PR_TASK_ISOLATION_GET_SIG(flags) ?: SIGKILL,
	};

	stop_isolation(task);
	send_sig_info(info.si_signo, &info, task);
}

/* Only a few syscalls are valid once we are in task isolation mode. */
static bool is_acceptable_syscall(int syscall)
{
	/* No need to incur an isolation signal if we are just exiting. */
	if (syscall == __NR_exit || syscall == __NR_exit_group)
		return true;

	/* Check to see if it's the prctl for isolation. */
	if (syscall == __NR_prctl) {
		unsigned long arg[SYSCALL_MAX_ARGS];

		syscall_get_arguments(current, current_pt_regs(), arg);
		if (arg[0] == PR_TASK_ISOLATION)
			return true;
	}

	return false;
}

/*
 * This routine is called from syscall entry, prevents most syscalls
 * from executing, and if needed raises a signal to notify the process.
 *
 * Note that we have to stop isolation before we even print a message
 * here, since otherwise we might end up reporting an interrupt due to
 * kicking the printk handling code, rather than reporting the true
 * cause of interrupt here.
 *
 * The message is not suppressed by previous remotely triggered
 * messages.
 */
int task_isolation_syscall(int syscall)
{
	struct task_struct *task = current;

	/*
	 * Check if by any chance syscall is being processed from
	 * isolated state without a call to
	 * task_isolation_kernel_enter() happening on entry
	 */
	if ((this_cpu_read(ll_isol_flags) & FLAG_LL_TASK_ISOLATION)) {
		/*
		 * If it did happen, call the function here, to
		 * prevent further problems from running in
		 * un-synchronized state
		 */
		task_isolation_kernel_enter();
		/* Report the problem */
		pr_task_isol_emerg(smp_processor_id(),
			"Isolation breaking was not detected on syscall\n");
		}
	/*
	 * Clear low-level isolation flags to avoid triggering
	 * a signal on return to userspace
	 */
	this_cpu_write(ll_isol_flags, 0);

	if (is_acceptable_syscall(syscall)) {
		stop_isolation(task);
		return 0;
	}

	send_isolation_signal(task);

	pr_task_isol_warn(smp_processor_id(),
			  "task_isolation lost due to syscall %d\n",
			  syscall);

	syscall_set_return_value(task, current_pt_regs(), -ERESTARTNOINTR, -1);
	return -1;
}

/*
 * This routine is called from the code responsible for exiting to user mode,
 * before the point when thread flags are checked for pending work.
 * That function must be called if the current task is isolated, because
 * TIF_TASK_ISOLATION must trigger a call to it.
 */
void task_isolation_before_pending_work_check(void)
{
	int cpu;
	unsigned long flags;

	/* Handle isolation breaking */
	if ((this_cpu_read(ll_isol_flags) & FLAG_LL_TASK_ISOLATION_BROKEN)
	    != 0) {
		/*
		 * Clear low-level isolation flags to avoid triggering
		 * a signal again
		 */
		this_cpu_write(ll_isol_flags, 0);
		/* Send signal to notify about isolation breaking */
		send_isolation_signal(current);
		/* Produce generic message about lost isolation */
		pr_task_isol_warn(smp_processor_id(), "task_isolation lost\n");
	}

	/*
	 * If this CPU is in the map of CPUs with cleanup pending,
	 * remove it from the map and call cleanup
	 */
	spin_lock_irqsave(&task_isolation_cleanup_lock, flags);

	cpu = smp_processor_id();

	if (cpumask_test_cpu(cpu, task_isolation_cleanup_map)) {
		cpumask_clear_cpu(cpu, task_isolation_cleanup_map);
		spin_unlock_irqrestore(&task_isolation_cleanup_lock, flags);
		task_isolation_cpu_cleanup();
	} else
		spin_unlock_irqrestore(&task_isolation_cleanup_lock, flags);
}

/*
 * Called before we wake up a task that has a signal to process.
 * Needs to be done to handle interrupts that trigger signals, which
 * we don't catch with task_isolation_interrupt() hooks.
 *
 * This message is also suppressed if there was already a remotely
 * caused message about the same isolation breaking event.
 */
void _task_isolation_signal(struct task_struct *task)
{
	struct isol_task_desc *desc;
	int ind, cpu;
	bool do_warn = (task->task_isolation_state == STATE_ISOLATED);

	cpu = task_cpu(task);
	desc = &per_cpu(isol_task_descs, cpu);
	ind = atomic_read(&desc->curr_index) & 1;
	if (desc->warned[ind])
		do_warn = false;

	stop_isolation(task);

	if (do_warn) {
		pr_warn("isolation: %s/%d/%d (cpu %d): task_isolation lost due to signal\n",
			task->comm, task->tgid, task->pid, cpu);
	}
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

/*
 * Cleanup procedure. The call to this procedure may be delayed.
 */
void task_isolation_cpu_cleanup(void)
{
	kick_hrtimer();
}
