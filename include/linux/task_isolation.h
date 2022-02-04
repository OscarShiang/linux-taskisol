/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_TASK_ISOL_H
#define __LINUX_TASK_ISOL_H

#ifdef CONFIG_TASK_ISOLATION

struct task_isol_info {
	/* Which features have been configured */
	u64 conf_mask;
	/* Which features are active */
	u64 active_mask;
	/* Quiesce mask */
	u64 quiesce_mask;

	/* Oneshot mask */
	u64 oneshot_mask;

	u8 inherit_mask;
};

extern void __task_isol_free(struct task_struct *tsk);

static inline void task_isol_free(struct task_struct *tsk)
{
	if (tsk->task_isol_info)
		__task_isol_free(tsk);
}

int prctl_task_isol_feat_get(unsigned long arg2, unsigned long arg3,
			     unsigned long arg4, unsigned long arg5);
int prctl_task_isol_cfg_get(unsigned long arg2, unsigned long arg3,
			    unsigned long arg4, unsigned long arg5);
int prctl_task_isol_cfg_set(unsigned long arg2, unsigned long arg3,
			    unsigned long arg4, unsigned long arg5);
int prctl_task_isol_activate_get(unsigned long arg2, unsigned long arg3,
				 unsigned long arg4, unsigned long arg5);
int prctl_task_isol_activate_set(unsigned long arg2, unsigned long arg3,
				 unsigned long arg4, unsigned long arg5);

int __copy_task_isol(struct task_struct *tsk);

#else

static inline void task_isol_free(struct task_struct *tsk)
{
}

static inline int prctl_task_isol_feat_get(unsigned long arg2,
					   unsigned long arg3,
					   unsigned long arg4,
					   unsigned long arg5)
{
	return -EOPNOTSUPP;
}

static inline int prctl_task_isol_cfg_get(unsigned long arg2,
					   unsigned long arg3,
					   unsigned long arg4,
					   unsigned long arg5)
{
	return -EOPNOTSUPP;
}

static inline int prctl_task_isol_cfg_set(unsigned long arg2,
					  unsigned long arg3,
					  unsigned long arg4,
					  unsigned long arg5)
{
	return -EOPNOTSUPP;
}

static inline int prctl_task_isol_activate_get(unsigned long arg2,
					       unsigned long arg3,
					       unsigned long arg4,
					       unsigned long arg5)
{
	return -EOPNOTSUPP;
}

static inline int prctl_task_isol_activate_set(unsigned long arg2,
					       unsigned long arg3,
					       unsigned long arg4,
					       unsigned long arg5)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_TASK_ISOLATION */

#endif /* __LINUX_TASK_ISOL_H */
