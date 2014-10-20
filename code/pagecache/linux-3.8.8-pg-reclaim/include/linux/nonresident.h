#ifndef _LINUX_NONRESIDENT_CART_H_
#define _LINUX_NONRESIDENT_CART_H_

#ifdef __KERNEL__

#include <linux/fs.h>
#include <linux/preempt.h>
#include <linux/percpu.h>

#define NR_b1		0
#define NR_b2		1
#define NR_free		2

#define NR_listid	3
#define NR_found	0x80000000

extern int nonresident_put(struct address_space *, unsigned long, int, int);
extern int nonresident_get(struct address_space *, unsigned long);
extern unsigned int nonresident_total(void);
extern void nonresident_init(void);
extern void nonresident_set_policy(int);
extern int nonresident_get_policy(void);
extern void nonres_forget(struct address_space *, unsigned long);

DECLARE_PER_CPU(unsigned long[4], nonres_count);

static inline unsigned long nonresident_count(int listid)
{
	unsigned long count;
	preempt_disable();
	count = __sum_cpu_var(unsigned long, nonres_count[listid]);
	preempt_enable();
	return count;
}

#endif /* __KERNEL__ */
#endif /* _LINUX_NONRESIDENT_CART_H_ */
