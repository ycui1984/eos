#ifndef __LINUX_SPINLOCK_TYPES_H
#define __LINUX_SPINLOCK_TYPES_H

/*
 * include/linux/spinlock_types.h - generic spinlock type definitions
 *                                  and initializers
 *
 * portions Copyright 2005, Red Hat, Inc., Ingo Molnar
 * Released under the General Public License (GPL).
 */

#if defined(CONFIG_SMP)
# include <asm/spinlock_types.h>
#else
# include <linux/spinlock_types_up.h>
#endif

#include <linux/lockdep.h>

typedef struct raw_spinlock {
	arch_spinlock_t raw_lock;
#ifdef CONFIG_GENERIC_LOCKBREAK
	unsigned int break_lock;
#endif
#ifdef CONFIG_DEBUG_SPINLOCK
	unsigned int magic, owner_cpu;
	void *owner;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map;
#endif
} raw_spinlock_t;

#define SPINLOCK_MAGIC		0xdead4ead

#define SPINLOCK_OWNER_INIT	((void *)-1L)

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define SPIN_DEP_MAP_INIT(lockname)	.dep_map = { .name = #lockname }
#else
# define SPIN_DEP_MAP_INIT(lockname)
#endif

#ifdef CONFIG_DEBUG_SPINLOCK
# define SPIN_DEBUG_INIT(lockname)		\
	.magic = SPINLOCK_MAGIC,		\
	.owner_cpu = -1,			\
	.owner = SPINLOCK_OWNER_INIT,
#else
# define SPIN_DEBUG_INIT(lockname)
#endif

#define __RAW_SPIN_LOCK_INITIALIZER(lockname)	\
	{					\
	.raw_lock = __ARCH_SPIN_LOCK_UNLOCKED,	\
	SPIN_DEBUG_INIT(lockname)		\
	SPIN_DEP_MAP_INIT(lockname) }

#define __RAW_SPIN_LOCK_UNLOCKED(lockname)	\
	(raw_spinlock_t) __RAW_SPIN_LOCK_INITIALIZER(lockname)

#define DEFINE_RAW_SPINLOCK(x)	raw_spinlock_t x = __RAW_SPIN_LOCK_UNLOCKED(x)

typedef struct spinlock {
	union {
		struct raw_spinlock rlock;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define LOCK_PADSIZE (offsetof(struct raw_spinlock, dep_map))
		struct {
			u8 __padding[LOCK_PADSIZE];
			struct lockdep_map dep_map;
		};
#endif
	};
} spinlock_t;

#define __SPIN_LOCK_INITIALIZER(lockname) \
	{ { .rlock = __RAW_SPIN_LOCK_INITIALIZER(lockname) } }

#define __SPIN_LOCK_UNLOCKED(lockname) \
	(spinlock_t ) __SPIN_LOCK_INITIALIZER(lockname)

#define DEFINE_SPINLOCK(x)	spinlock_t x = __SPIN_LOCK_UNLOCKED(x)

#include <linux/rwlock_types.h>

#define PNTINV          1

#define UNLOCKED        0

#define CACHELINE       64
#define COMPILER_BARRIER asm volatile("" ::: "memory")

#define STATE_TAS       0
#define STATE_QUEUE     1
#define STATE_TICKET    2

extern volatile int method_tuner;
extern volatile int val_tuner;

enum release_mode {
        TAS,
        TAS_TO_QUEUE,
        TAS_TO_TICKET,
        TICKET,
        TICKET_TO_QUEUE,
        TICKET_TO_TAS,
        QUEUE,
        QUEUE_TO_TAS,
        QUEUE_TO_TICKET,
	TRYLOCK_FAILED
};

enum lock_mode {
        MODE_TAS,
        MODE_QUEUE,
        MODE_TICKET
};

enum node_status {
        WAITING,
        GOT_LOCK,
        INVALID
};

struct qnode {
        volatile struct qnode* next __attribute__((__aligned__(CACHELINE)));
        volatile enum node_status status;
        char __pad[0] __attribute__((__aligned__(CACHELINE)));
};

typedef struct reactive_spinlock {
        union {
                __ticketpair_t head_tail;
                struct __reactive_raw_tickets {
                        __ticket_t head, tail;
                } tickets;
        };
        volatile bool isvalid;
} reactive_spinlock_t;

struct lock {
        uint8_t tas_lock __attribute__((__aligned__(CACHELINE)));
        struct qnode* queue_tail;
        reactive_spinlock_t ticket_lock;
        enum lock_mode mode __attribute__((__aligned__(CACHELINE)));
};
#endif /* __LINUX_SPINLOCK_TYPES_H */
