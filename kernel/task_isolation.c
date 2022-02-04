// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Implementation of task isolation.
 *
 * Authors:
 *   Chris Metcalf <cmetcalf@mellanox.com>
 *   Alex Belits <abelits@belits.com>
 *   Yuri Norov <ynorov@marvell.com>
 *   Marcelo Tosatti <mtosatti@redhat.com>
 */

#include <linux/sched.h>
#include <linux/prctl.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/vmstat.h>
#include <linux/preempt.h>
#include <linux/task_isolation.h>

void __task_isol_exit(struct task_struct *tsk)
{
	struct task_isol_info *i;

	i = tsk->task_isol_info;
	if (!i)
		return;

	static_key_slow_dec(&vmstat_sync_enabled);

	preempt_notifier_unregister(&i->preempt_notifier);
	preempt_notifier_dec();
}

void __task_isol_free(struct task_struct *tsk)
{
	if (!tsk->task_isol_info)
		return;
	kfree(tsk->task_isol_info);
	tsk->task_isol_info = NULL;
}

static void task_isol_sched_in(struct preempt_notifier *pn, int cpu)
{
	vmstat_dirty_to_thread_flag();
}

static void task_isol_sched_out(struct preempt_notifier *pn,
				struct task_struct *next)
{
}

static __read_mostly struct preempt_ops task_isol_preempt_ops = {
	.sched_in	= task_isol_sched_in,
	.sched_out	= task_isol_sched_out,
};

static struct task_isol_info *task_isol_alloc_context(void)
{
	struct task_isol_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (unlikely(!info))
		return ERR_PTR(-ENOMEM);

	preempt_notifier_inc();
	preempt_notifier_init(&info->preempt_notifier, &task_isol_preempt_ops);
	preempt_notifier_register(&info->preempt_notifier);

	preempt_disable();
	init_sync_vmstat();
	preempt_enable();

	static_key_slow_inc(&vmstat_sync_enabled);

	return info;
}

int prctl_task_isol_feat_get(unsigned long arg2, unsigned long arg3,
				  unsigned long arg4, unsigned long arg5)
{
	int ret;
	void __user *addr = (void __user *) arg3;

	switch (arg2) {
	case 0: {
		u64 supported_fmask = ISOL_F_QUIESCE;

		ret = 0;
		if (copy_to_user(addr, &supported_fmask, sizeof(u64)))
			ret = -EFAULT;

		return ret;
	}
	case ISOL_F_QUIESCE: {
		struct task_isol_quiesce_extensions *q_ext;

		q_ext = kzalloc(sizeof(struct task_isol_quiesce_extensions),
			 GFP_KERNEL);
		if (!q_ext)
			return -ENOMEM;

		q_ext->supported_quiesce_bits = ISOL_F_QUIESCE_VMSTATS;

		ret = 0;
		if (copy_to_user(addr, q_ext, sizeof(*q_ext)))
			ret = -EFAULT;
		kfree(q_ext);
		return ret;
	}
	default:
		break;
	}
	return -EINVAL;
}

static int cfg_inherit_get(unsigned long arg3, unsigned long arg4,
			   unsigned long arg5)
{
	struct task_isol_inherit_control *i_ctrl;
	int ret;
	void __user *addr = (void __user *) arg3;

	if (!current->task_isol_info)
		return -EINVAL;

	i_ctrl = kzalloc(sizeof(struct task_isol_inherit_control),
			 GFP_KERNEL);
	if (!i_ctrl)
		return -ENOMEM;

	i_ctrl->inherit_mask = current->task_isol_info->inherit_mask;

	ret = 0;
	if (copy_to_user(addr, i_ctrl, sizeof(*i_ctrl)))
		ret = -EFAULT;
	kfree(i_ctrl);

	return ret;
}

static int cfg_feat_get(unsigned long arg3, unsigned long arg4,
			unsigned long arg5)
{
	int ret = 0;

	switch (arg3) {
	case 0: {
		void __user *addr = (void __user *)arg4;
		u64 cfg_mask = 0;

		if (current->task_isol_info)
			cfg_mask = current->task_isol_info->conf_mask;

		if (copy_to_user(addr, &cfg_mask, sizeof(u64)))
			ret = -EFAULT;

		return ret;
	}
	case ISOL_F_QUIESCE: {
		struct task_isol_quiesce_control *i_qctrl;
		void __user *addr = (void __user *)arg5;

		if (arg4 != QUIESCE_CONTROL)
			return -EINVAL;

		i_qctrl = kzalloc(sizeof(struct task_isol_quiesce_control),
				  GFP_KERNEL);
		if (!i_qctrl)
			return -ENOMEM;

		if (current->task_isol_info)
			i_qctrl->quiesce_mask = current->task_isol_info->quiesce_mask;

		if (copy_to_user(addr, i_qctrl, sizeof(*i_qctrl)))
			ret = -EFAULT;

		kfree(i_qctrl);
		return ret;
	}
	default:
		break;
	}
	return -EINVAL;
}

int prctl_task_isol_cfg_get(unsigned long arg2, unsigned long arg3,
				 unsigned long arg4, unsigned long arg5)
{
	switch (arg2) {
	case I_CFG_FEAT:
		return cfg_feat_get(arg3, arg4, arg5);
	case I_CFG_INHERIT:
		return cfg_inherit_get(arg3, arg4, arg5);
	default:
		break;
	}
	return -EINVAL;
}

static int cfg_inherit_set(unsigned long arg3, unsigned long arg4,
			   unsigned long arg5)
{
	int ret = 0;
	struct task_isol_inherit_control *i_ctrl;
	const void __user *addr = (const void __user *)arg3;

	i_ctrl = kzalloc(sizeof(struct task_isol_inherit_control),
			 GFP_KERNEL);
	if (!i_ctrl)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(i_ctrl, addr, sizeof(*i_ctrl)))
		goto out_free;

	ret = -EINVAL;
	if (i_ctrl->inherit_mask & ~(ISOL_INHERIT_CONF|ISOL_INHERIT_ACTIVE))
		goto out_free;

	if (i_ctrl->inherit_mask & ISOL_INHERIT_ACTIVE)
		if (!(i_ctrl->inherit_mask & ISOL_INHERIT_CONF))
			goto out_free;

	if (!current->task_isol_info) {
		struct task_isol_info *task_isol_info;

		task_isol_info = task_isol_alloc_context();
		if (IS_ERR(task_isol_info)) {
			ret = PTR_ERR(task_isol_info);
			goto out_free;
		}
		current->task_isol_info = task_isol_info;
	}

	ret = 0;
	current->task_isol_info->inherit_mask = i_ctrl->inherit_mask;

out_free:
	kfree(i_ctrl);

	return ret;
}

static int cfg_feat_quiesce_set(unsigned long arg4, unsigned long arg5)
{
	struct task_isol_info *task_isol_info;
	struct task_isol_quiesce_control *i_qctrl;
	int ret = 0;
	const void __user *addr = (const void __user *)arg5;

	if (arg4 != QUIESCE_CONTROL)
		return -EINVAL;

	i_qctrl = kzalloc(sizeof(struct task_isol_quiesce_control),
			 GFP_KERNEL);
	if (!i_qctrl)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(i_qctrl, addr, sizeof(*i_qctrl)))
		goto out_free;

	ret = -EINVAL;
	if (i_qctrl->flags != 0)
		goto out_free;

	if (i_qctrl->quiesce_mask != ISOL_F_QUIESCE_VMSTATS &&
	    i_qctrl->quiesce_mask != 0)
		goto out_free;

	if ((~i_qctrl->quiesce_mask & i_qctrl->quiesce_oneshot_mask) != 0)
		goto out_free;

	/* current->task_isol_info is only allocated/freed from task
	 * context.
	 */
	if (!current->task_isol_info) {
		task_isol_info = task_isol_alloc_context();
		if (IS_ERR(task_isol_info)) {
			ret = PTR_ERR(task_isol_info);
			goto out_free;
		}
		current->task_isol_info = task_isol_info;
	}

	task_isol_info = current->task_isol_info;

	task_isol_info->quiesce_mask = i_qctrl->quiesce_mask;
	task_isol_info->oneshot_mask = i_qctrl->quiesce_oneshot_mask;
	task_isol_info->conf_mask |= ISOL_F_QUIESCE;
	if (task_isol_info->quiesce_mask & ISOL_F_QUIESCE_VMSTATS)
		set_thread_flag(TIF_TASK_ISOL);

	ret = 0;

out_free:
	kfree(i_qctrl);

	return ret;
}

int prctl_task_isol_cfg_set(unsigned long arg2, unsigned long arg3,
				 unsigned long arg4, unsigned long arg5)
{
	switch (arg2) {
	case I_CFG_FEAT:
		switch (arg3) {
		case ISOL_F_QUIESCE:
			return cfg_feat_quiesce_set(arg4, arg5);
		default:
			break;
		}
		break;
	case I_CFG_INHERIT:
		return cfg_inherit_set(arg3, arg4, arg5);
	default:
		break;
	}
	return -EINVAL;
}

int __copy_task_isol(struct task_struct *tsk)
{
	struct task_isol_info *info, *new_info;

	info = current->task_isol_info;
	if (!(info->inherit_mask & (ISOL_INHERIT_CONF|ISOL_INHERIT_ACTIVE)))
		return 0;

	new_info = task_isol_alloc_context();
	if (IS_ERR(new_info))
		return PTR_ERR(new_info);

	new_info->inherit_mask = info->inherit_mask;

	if (info->inherit_mask & ISOL_INHERIT_CONF) {
		new_info->quiesce_mask = info->quiesce_mask;
		new_info->conf_mask = info->conf_mask;
		new_info->oneshot_mask = info->oneshot_mask;
	}

	if (info->inherit_mask & ISOL_INHERIT_ACTIVE)
		new_info->active_mask = info->active_mask;

	tsk->task_isol_info = new_info;
	set_ti_thread_flag(task_thread_info(tsk), TIF_TASK_ISOL);

	return 0;
}

int prctl_task_isol_activate_set(unsigned long arg2, unsigned long arg3,
				      unsigned long arg4, unsigned long arg5)
{
	int ret;
	struct task_isol_info *task_isol_info;
	u64 active_mask;
	const void __user *addr_mask = (const void __user *)arg2;

	ret = -EFAULT;
	if (copy_from_user(&active_mask, addr_mask, sizeof(u64)))
		goto out;

	ret = -EINVAL;
	if (active_mask != ISOL_F_QUIESCE && active_mask != 0)
		return ret;

	task_isol_info = current->task_isol_info;
	if (!task_isol_info)
		return ret;

	task_isol_info->active_mask = active_mask;
	set_thread_flag(TIF_TASK_ISOL);
	ret = 0;

out:
	return ret;
}

int prctl_task_isol_activate_get(unsigned long arg2, unsigned long arg3,
				      unsigned long arg4, unsigned long arg5)
{
	struct task_isol_info *task_isol_info;
	void __user *addr_mask = (void __user *)arg2;

	task_isol_info = current->task_isol_info;
	if (!task_isol_info)
		return -EINVAL;

	if (copy_to_user(addr_mask, &task_isol_info->active_mask, sizeof(u64)))
		return -EFAULT;

	return 0;
}

void task_isol_exit_to_user_mode(void)
{
	struct task_isol_info *i;

	clear_thread_flag(TIF_TASK_ISOL);

	i = current->task_isol_info;
	if (!i)
		return;

	if (i->active_mask != ISOL_F_QUIESCE)
		return;

	if (i->quiesce_mask & ISOL_F_QUIESCE_VMSTATS) {
		sync_vmstat();
		if (i->oneshot_mask & ISOL_F_QUIESCE_VMSTATS)
			i->quiesce_mask &= ~ISOL_F_QUIESCE_VMSTATS;
	}
}
EXPORT_SYMBOL_GPL(task_isol_exit_to_user_mode);
