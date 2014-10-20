#include <linux/linkage.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

extern void _init_lock(struct lock *lock);
extern enum release_mode _acquire_mixed_lock(struct lock *lock, volatile struct qnode* node);
extern void _release_mixed_lock(struct lock *lock, volatile struct qnode* node, enum release_mode mode);

#ifdef SUBSYS_SPINLOCK
extern void backoff_spin_lock(spinlock_t *lock);
extern void backoff_spin_unlock(spinlock_t *lock);
#endif

long single_ctr = 0;

#ifndef SUBSYS_MIXEDLOCK
DEFINE_SPINLOCK(single_ctr_lock);

#else
struct lock_wrapper {
	struct lock mylock;
	int pad[0] __attribute__((aligned(64)));
};
struct lock_wrapper test_lock[48];
int init_locks = 0;
#endif

#define BENCHMARK	1
#ifdef SUBSYS_MIXEDLOCK
#define CHECKSTATE	2
#define INITLOCK	3
#endif

#ifdef SUBSYS_MIXEDLOCK
int init_lock_array(void)
{
	int i;
	for (i = 0; i<48; i++) 
		init_lock(&(test_lock[i].mylock));

	return 0;
}
#endif

long benchmark(int n, int cpuid)
{
	int loops;
#ifdef SUBSYS_MIXEDLOCK
	struct qnode node;
#endif
	
        if (n < 0) {
                printk("single ctr parameter is smaller than 0\n");
                return -1;
        }

	if (0 == init_locks) {
		printk("init locks first\n");	
		return -1;
	}

        for (loops = 0; loops < n; loops ++) {
#ifdef SUBSYS_SPINLOCK
                backoff_spin_lock(&single_ctr_lock);
#elif defined (SUBSYS_MIXEDLOCK)
		enum release_mode mode = acquire_mixed_lock(&(test_lock[cpuid].mylock), (volatile struct qnode*)(&node));
#else
		spin_lock(&single_ctr_lock);
#endif
                /* single_ctr ++; */
#ifdef SUBSYS_SPINLOCK
                backoff_spin_unlock(&single_ctr_lock);
#elif defined(SUBSYS_MIXEDLOCK)
		release_mixed_lock(&(test_lock[cpuid].mylock), (volatile struct qnode*)(&node), mode);
#else
		spin_unlock(&single_ctr_lock);
#endif
        } 

        return 0;
}

asmlinkage long sys_single_ctr(int id, int n, int cpuid)
{
	if (BENCHMARK == id) 
		return benchmark(n, cpuid);
#ifdef SUBSYS_MIXEDLOCK
	if (INITLOCK == id) {
		init_locks = 1;
		printk("single ctr\t locks init\n");
		return init_lock_array();
	}
	if (CHECKSTATE == id) {
		return 0;
	}
#endif
	return -1;		
}

