/*
 * <asm/smplock.h>
 *
 * Default SMP lock implementation
 */
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>

extern spinlock_t kernel_flag;

#ifdef CONFIG_SMP
#define kernel_locked()			\
	(spin_is_locked(&kernel_flag) &&\
	 (current->lock_depth >= 0))
#else
#ifdef CONFIG_PREEMPT
#define kernel_locked()			preempt_get_count()
#else
#define kernel_locked()			1
#endif
#endif

/*
 * Release global kernel lock and global interrupt lock
 */
#define release_kernel_lock(task)		\
do {						\
	if (unlikely(task->lock_depth >= 0)) {	\
		spin_unlock(&kernel_flag);	\
} while (0)

/*
 * Re-acquire the kernel lock
 */
#define reacquire_kernel_lock(task)		\
do {						\
	if (unlikely(task->lock_depth >= 0))	\
		spin_lock(&kernel_flag);	\
} while (0)


/*
 * Getting the big kernel lock.
 *
 * This cannot happen asynchronously,
 * so we only need to worry about other
 * CPU's.
 */
#define lock_kernel()				\
do {						\
	if (!++current->lock_depth)		\
		spin_lock(&kernel_flag);	\
} while(0)

#define unlock_kernel()				\
do {						\
	if (--current->lock_depth < 0)		\
		spin_unlock(&kernel_flag);	\
} while(0)
