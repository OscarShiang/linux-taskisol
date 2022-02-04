/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __TASK_ISOL_H
#define __TASK_ISOL_H

int task_isol_setup(int oneshot);

int task_isol_activate_set(unsigned long long mask);

#endif /* __TASK_ISOL_H */
