#include <linux/linkage.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/statedef.h>

#ifdef SUBSYS_SPINLOCK
extern void backoff_spin_lock(spinlock_t *lock);
extern void backoff_spin_unlock(spinlock_t *lock);
#endif

long single_ctr = 0;

#ifndef SUBSYS_MIXEDLOCK
DEFINE_SPINLOCK(single_ctr_lock);

#else
struct lock test_lock = {
	.tas_lock = UNLOCKED,
	.queue_tail = (struct qnode*)PNTINV,
	.ticket_lock.isvalid = false,
	.mode = MODE_TAS
};
#endif

#define BENCHMARK	1
#ifdef SUBSYS_MIXEDLOCK
#define CHECKSTATE	2
#endif

long benchmark(int n)
{
	int loops;
#ifdef SUBSYS_MIXEDLOCK
	struct qnode node;
#endif
	
        if (n < 0) {
                printk("single ctr parameter is smaller than 0\n");
                return -1;
        }

        for (loops = 0; loops < n; loops ++) {
#ifdef SUBSYS_SPINLOCK
                backoff_spin_lock(&single_ctr_lock);
#elif defined (SUBSYS_MIXEDLOCK)
		enum release_mode mode = acquire_mixed_lock(&test_lock, (volatile struct qnode*)(&node));
#else
		spin_lock(&single_ctr_lock);
#endif
                single_ctr ++;
#ifdef SUBSYS_SPINLOCK
                backoff_spin_unlock(&single_ctr_lock);
#elif defined(SUBSYS_MIXEDLOCK)
		release_mixed_lock(&test_lock, (volatile struct qnode*)(&node), mode);
#else
		spin_unlock(&single_ctr_lock);
#endif
        } 

        return 0;
}

#ifdef SUBSYS_MIXEDLOCK
void print_state(void) 
{
	printk("tas_lock:%d\nhead of ticket_lock:%u\ntail of ticket_lock:%u\nmode:%d\n", test_lock.tas_lock, test_lock.ticket_lock.tickets.head, test_lock.ticket_lock.tickets.tail, test_lock.mode);
}
#endif

asmlinkage long sys_single_ctr(int id, int n)
{
	if (BENCHMARK == id) 
		return benchmark(n);
#ifdef SUBSYS_MIXEDLOCK
	if (CHECKSTATE == id) {
		print_state();
		return 0;
	}
#endif
	return -1;		
}

