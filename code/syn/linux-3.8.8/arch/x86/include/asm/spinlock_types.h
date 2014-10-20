#ifndef _ASM_X86_SPINLOCK_TYPES_H
#define _ASM_X86_SPINLOCK_TYPES_H

#ifndef __LINUX_SPINLOCK_TYPES_H
# error "please don't include this file directly"
#endif

#include <linux/statedef.h>
#include <linux/types.h>

#if (CONFIG_NR_CPUS < 256)
typedef u8  __ticket_t;
typedef u16 __ticketpair_t;
#else
typedef u16 __ticket_t;
typedef u32 __ticketpair_t;
#endif

#define TICKET_SHIFT	(sizeof(__ticket_t) * 8)

typedef struct arch_spinlock {
	union {
		__ticketpair_t head_tail;
		struct __raw_tickets {
			__ticket_t head, tail;
		} tickets;
	};
#if defined (SUBSYS_SPINLOCK) && defined (AUTO_SPINLOCK) 
        u32 delay __attribute__((__aligned__(64)));
#endif
} arch_spinlock_t;

#ifdef SUBSYS_SPINLOCK

#ifdef AUTO_SPINLOCK
#define __ARCH_SPIN_LOCK_UNLOCKED	{ { 0 }, 1 << 8 }
#else
#define __ARCH_SPIN_LOCK_UNLOCKED	{ { 0 } }
#endif

#else
#define __ARCH_SPIN_LOCK_UNLOCKED	{ { 0 } }
#endif

#include <asm/rwlock.h>

#endif /* _ASM_X86_SPINLOCK_TYPES_H */
