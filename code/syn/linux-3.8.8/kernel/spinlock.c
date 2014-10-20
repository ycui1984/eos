/*
 * Copyright (2004) Linus Torvalds
 *
 * Author: Zwane Mwaikambo <zwane@fsmlabs.com>
 *
 * Copyright (2004, 2005) Ingo Molnar
 *
 * This file contains the spinlock/rwlock implementations for the
 * SMP and the DEBUG_SPINLOCK cases. (UP-nondebug inlines them)
 *
 * Note that some architectures have special knowledge about the
 * stack frames of these functions in their profile_pc. If you
 * change anything significant here that could change the stack
 * frame contact the architecture maintainers.
 */

#include <linux/linkage.h>
#include <linux/preempt.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>
#include <linux/export.h>

/*
 * If lockdep is enabled then we use the non-preemption spin-ops
 * even on CONFIG_PREEMPT, because lockdep assumes that interrupts are
 * not re-enabled during lock-acquire (which the preempt-spin-ops do):
 */
#if !defined(CONFIG_GENERIC_LOCKBREAK) || defined(CONFIG_DEBUG_LOCK_ALLOC)
/*
 * The __lock_function inlines are taken from
 * include/linux/spinlock_api_smp.h
 */
#else
#define raw_read_can_lock(l)	read_can_lock(l)
#define raw_write_can_lock(l)	write_can_lock(l)
/*
 * We build the __lock_function inlines here. They are too large for
 * inlining all over the place, but here is only one user per function
 * which embedds them into the calling _lock_function below.
 *
 * This could be a long-held lock. We both prepare to spin for a long
 * time (making _this_ CPU preemptable if possible), and we also signal
 * towards that other CPU that it should break the lock ASAP.
 */
#define BUILD_LOCK_OPS(op, locktype)					\
void __lockfunc __raw_##op##_lock(locktype##_t *lock)			\
{									\
	for (;;) {							\
		preempt_disable();					\
		if (likely(do_raw_##op##_trylock(lock)))		\
			break;						\
		preempt_enable();					\
									\
		if (!(lock)->break_lock)				\
			(lock)->break_lock = 1;				\
		while (!raw_##op##_can_lock(lock) && (lock)->break_lock)\
			arch_##op##_relax(&lock->raw_lock);		\
	}								\
	(lock)->break_lock = 0;						\
}									\
									\
unsigned long __lockfunc __raw_##op##_lock_irqsave(locktype##_t *lock)	\
{									\
	unsigned long flags;						\
									\
	for (;;) {							\
		preempt_disable();					\
		local_irq_save(flags);					\
		if (likely(do_raw_##op##_trylock(lock)))		\
			break;						\
		local_irq_restore(flags);				\
		preempt_enable();					\
									\
		if (!(lock)->break_lock)				\
			(lock)->break_lock = 1;				\
		while (!raw_##op##_can_lock(lock) && (lock)->break_lock)\
			arch_##op##_relax(&lock->raw_lock);		\
	}								\
	(lock)->break_lock = 0;						\
	return flags;							\
}									\
									\
void __lockfunc __raw_##op##_lock_irq(locktype##_t *lock)		\
{									\
	_raw_##op##_lock_irqsave(lock);					\
}									\
									\
void __lockfunc __raw_##op##_lock_bh(locktype##_t *lock)		\
{									\
	unsigned long flags;						\
									\
	/*							*/	\
	/* Careful: we must exclude softirqs too, hence the	*/	\
	/* irq-disabling. We use the generic preemption-aware	*/	\
	/* function:						*/	\
	/**/								\
	flags = _raw_##op##_lock_irqsave(lock);				\
	local_bh_disable();						\
	local_irq_restore(flags);					\
}									\

/*
 * Build preemption-friendly versions of the following
 * lock-spinning functions:
 *
 *         __[spin|read|write]_lock()
 *         __[spin|read|write]_lock_irq()
 *         __[spin|read|write]_lock_irqsave()
 *         __[spin|read|write]_lock_bh()
 */
BUILD_LOCK_OPS(spin, raw_spinlock);
BUILD_LOCK_OPS(read, rwlock);
BUILD_LOCK_OPS(write, rwlock);

#endif

#ifndef CONFIG_INLINE_SPIN_TRYLOCK
int __lockfunc _raw_spin_trylock(raw_spinlock_t *lock)
{
	return __raw_spin_trylock(lock);
}
EXPORT_SYMBOL(_raw_spin_trylock);
#endif

#ifndef CONFIG_INLINE_SPIN_TRYLOCK_BH
int __lockfunc _raw_spin_trylock_bh(raw_spinlock_t *lock)
{
	return __raw_spin_trylock_bh(lock);
}
EXPORT_SYMBOL(_raw_spin_trylock_bh);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK
void __lockfunc _raw_spin_lock(raw_spinlock_t *lock)
{
	__raw_spin_lock(lock);
}
EXPORT_SYMBOL(_raw_spin_lock);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_IRQSAVE
unsigned long __lockfunc _raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
	return __raw_spin_lock_irqsave(lock);
}
EXPORT_SYMBOL(_raw_spin_lock_irqsave);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_IRQ
void __lockfunc _raw_spin_lock_irq(raw_spinlock_t *lock)
{
	__raw_spin_lock_irq(lock);
}
EXPORT_SYMBOL(_raw_spin_lock_irq);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_BH
void __lockfunc _raw_spin_lock_bh(raw_spinlock_t *lock)
{
	__raw_spin_lock_bh(lock);
}
EXPORT_SYMBOL(_raw_spin_lock_bh);
#endif

#ifdef CONFIG_UNINLINE_SPIN_UNLOCK
void __lockfunc _raw_spin_unlock(raw_spinlock_t *lock)
{
	__raw_spin_unlock(lock);
}
EXPORT_SYMBOL(_raw_spin_unlock);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_IRQRESTORE
void __lockfunc _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags)
{
	__raw_spin_unlock_irqrestore(lock, flags);
}
EXPORT_SYMBOL(_raw_spin_unlock_irqrestore);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_IRQ
void __lockfunc _raw_spin_unlock_irq(raw_spinlock_t *lock)
{
	__raw_spin_unlock_irq(lock);
}
EXPORT_SYMBOL(_raw_spin_unlock_irq);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_BH
void __lockfunc _raw_spin_unlock_bh(raw_spinlock_t *lock)
{
	__raw_spin_unlock_bh(lock);
}
EXPORT_SYMBOL(_raw_spin_unlock_bh);
#endif

#ifndef CONFIG_INLINE_READ_TRYLOCK
int __lockfunc _raw_read_trylock(rwlock_t *lock)
{
	return __raw_read_trylock(lock);
}
EXPORT_SYMBOL(_raw_read_trylock);
#endif

#ifndef CONFIG_INLINE_READ_LOCK
void __lockfunc _raw_read_lock(rwlock_t *lock)
{
	__raw_read_lock(lock);
}
EXPORT_SYMBOL(_raw_read_lock);
#endif

#ifndef CONFIG_INLINE_READ_LOCK_IRQSAVE
unsigned long __lockfunc _raw_read_lock_irqsave(rwlock_t *lock)
{
	return __raw_read_lock_irqsave(lock);
}
EXPORT_SYMBOL(_raw_read_lock_irqsave);
#endif

#ifndef CONFIG_INLINE_READ_LOCK_IRQ
void __lockfunc _raw_read_lock_irq(rwlock_t *lock)
{
	__raw_read_lock_irq(lock);
}
EXPORT_SYMBOL(_raw_read_lock_irq);
#endif

#ifndef CONFIG_INLINE_READ_LOCK_BH
void __lockfunc _raw_read_lock_bh(rwlock_t *lock)
{
	__raw_read_lock_bh(lock);
}
EXPORT_SYMBOL(_raw_read_lock_bh);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK
void __lockfunc _raw_read_unlock(rwlock_t *lock)
{
	__raw_read_unlock(lock);
}
EXPORT_SYMBOL(_raw_read_unlock);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK_IRQRESTORE
void __lockfunc _raw_read_unlock_irqrestore(rwlock_t *lock, unsigned long flags)
{
	__raw_read_unlock_irqrestore(lock, flags);
}
EXPORT_SYMBOL(_raw_read_unlock_irqrestore);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK_IRQ
void __lockfunc _raw_read_unlock_irq(rwlock_t *lock)
{
	__raw_read_unlock_irq(lock);
}
EXPORT_SYMBOL(_raw_read_unlock_irq);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK_BH
void __lockfunc _raw_read_unlock_bh(rwlock_t *lock)
{
	__raw_read_unlock_bh(lock);
}
EXPORT_SYMBOL(_raw_read_unlock_bh);
#endif

#ifndef CONFIG_INLINE_WRITE_TRYLOCK
int __lockfunc _raw_write_trylock(rwlock_t *lock)
{
	return __raw_write_trylock(lock);
}
EXPORT_SYMBOL(_raw_write_trylock);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK
void __lockfunc _raw_write_lock(rwlock_t *lock)
{
	__raw_write_lock(lock);
}
EXPORT_SYMBOL(_raw_write_lock);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK_IRQSAVE
unsigned long __lockfunc _raw_write_lock_irqsave(rwlock_t *lock)
{
	return __raw_write_lock_irqsave(lock);
}
EXPORT_SYMBOL(_raw_write_lock_irqsave);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK_IRQ
void __lockfunc _raw_write_lock_irq(rwlock_t *lock)
{
	__raw_write_lock_irq(lock);
}
EXPORT_SYMBOL(_raw_write_lock_irq);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK_BH
void __lockfunc _raw_write_lock_bh(rwlock_t *lock)
{
	__raw_write_lock_bh(lock);
}
EXPORT_SYMBOL(_raw_write_lock_bh);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK
void __lockfunc _raw_write_unlock(rwlock_t *lock)
{
	__raw_write_unlock(lock);
}
EXPORT_SYMBOL(_raw_write_unlock);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK_IRQRESTORE
void __lockfunc _raw_write_unlock_irqrestore(rwlock_t *lock, unsigned long flags)
{
	__raw_write_unlock_irqrestore(lock, flags);
}
EXPORT_SYMBOL(_raw_write_unlock_irqrestore);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK_IRQ
void __lockfunc _raw_write_unlock_irq(rwlock_t *lock)
{
	__raw_write_unlock_irq(lock);
}
EXPORT_SYMBOL(_raw_write_unlock_irq);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK_BH
void __lockfunc _raw_write_unlock_bh(rwlock_t *lock)
{
	__raw_write_unlock_bh(lock);
}
EXPORT_SYMBOL(_raw_write_unlock_bh);
#endif

#ifdef CONFIG_DEBUG_LOCK_ALLOC

void __lockfunc _raw_spin_lock_nested(raw_spinlock_t *lock, int subclass)
{
	preempt_disable();
	spin_acquire(&lock->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}
EXPORT_SYMBOL(_raw_spin_lock_nested);

unsigned long __lockfunc _raw_spin_lock_irqsave_nested(raw_spinlock_t *lock,
						   int subclass)
{
	unsigned long flags;

	local_irq_save(flags);
	preempt_disable();
	spin_acquire(&lock->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED_FLAGS(lock, do_raw_spin_trylock, do_raw_spin_lock,
				do_raw_spin_lock_flags, &flags);
	return flags;
}
EXPORT_SYMBOL(_raw_spin_lock_irqsave_nested);

void __lockfunc _raw_spin_lock_nest_lock(raw_spinlock_t *lock,
				     struct lockdep_map *nest_lock)
{
	preempt_disable();
	spin_acquire_nest(&lock->dep_map, 0, 0, nest_lock, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}
EXPORT_SYMBOL(_raw_spin_lock_nest_lock);

#endif

notrace int in_lock_functions(unsigned long addr)
{
	/* Linker adds these: start and end of __lockfunc functions */
	extern char __lock_text_start[], __lock_text_end[];

	return addr >= (unsigned long)__lock_text_start
	&& addr < (unsigned long)__lock_text_end;
}
EXPORT_SYMBOL(in_lock_functions);

static inline uint64_t read_tsc(void)
{
        uint64_t a, d;
        __asm __volatile("rdtsc" : "=a" (a), "=d" (d));
        return ((uint64_t) a) | (((uint64_t) d) << 32);
}

static inline void spin_delay(uint64_t cycles){

        uint64_t s = read_tsc();
        while (read_tsc() - s < cycles)
                cpu_relax();
}

static inline uint64_t fetch_and_store(struct lock* lock, uint64_t val)
{
        asm volatile ("xchgq %0, %1\n\t"
                        : "+m" (lock->queue_tail), "+r" (val)
                        :
                        : "memory", "cc");
        return val;
}

static inline uint64_t compare_and_swap(struct lock* lock, uint64_t cmpval, uint64_t newval)
{
    uint64_t out;
    asm volatile ("lock; cmpxchgq %2, %1"
                : "=a" (out), "+m" (lock->queue_tail)
                : "q" (newval), "0"(cmpval)
                : "cc");
    return out == cmpval;
}

static inline uint8_t test_and_set(volatile uint8_t *addr)
{
        uint8_t oldval;
        asm volatile ("xchgb %0, %1"
                : "=q"(oldval), "=m"(*addr)
                : "0"((unsigned char) 0xff), "m"(*addr) : "memory");

        return (uint8_t) oldval;
}

static enum release_mode acquire_tas(struct lock* lock, volatile struct qnode* node);
static enum release_mode acquire_queue(struct lock* lock, volatile struct qnode* node);

static enum release_mode acquire_ticket(struct lock* lock, volatile struct qnode* node)
{
        register struct __reactive_raw_tickets inc = { .tail = 1 };
        __ticket_t waiters_ahead;
        unsigned long loops;
        enum release_mode mode;

        inc = xadd(&((lock->ticket_lock).tickets), inc);

        for (;;) {
                if (inc.head == inc.tail) {
                        if (method_tuner == STATE_TAS)
                                mode = TICKET_TO_TAS;
                        else if (method_tuner == STATE_QUEUE)
                                mode = TICKET_TO_QUEUE;
                        else mode = TICKET;

                        barrier();
                        return mode;
                }

                if (false == lock->ticket_lock.isvalid) {

                        if (MODE_TAS == lock->mode) {
                                barrier();
                                return acquire_tas(lock, node);
                        } else if (MODE_QUEUE == lock->mode) {
                                barrier();
                                return acquire_queue(lock, node);
                        }
                }

                waiters_ahead = inc.tail - inc.head;
                loops = val_tuner * waiters_ahead;
                while (loops --)
                        cpu_relax();

                if (MODE_TAS == lock->mode)  {
                        barrier();
                        return acquire_tas(lock, node);
                }
                else if (MODE_QUEUE == lock->mode) {
                        barrier();
                        return acquire_queue(lock, node);
                }

                inc.head = ACCESS_ONCE((lock->ticket_lock).tickets.head);
        }
}

static enum release_mode acquire_ticket_trylock(struct lock* lock, volatile struct qnode* node)
{
	reactive_spinlock_t old, new;
	int ret; 

	if (false == lock->ticket_lock.isvalid) 
		return TRYLOCK_FAILED;

	old.tickets = ACCESS_ONCE((lock->ticket_lock).tickets);
	
	if (old.tickets.head != old.tickets.tail)
		return TRYLOCK_FAILED;

	new.head_tail = old.head_tail + (1 << TICKET_SHIFT);
	ret = (cmpxchg(&((lock->ticket_lock).head_tail), old.head_tail, new.head_tail) == old.head_tail);

	if (0 == ret) return TRYLOCK_FAILED;

        return TICKET;
}

static void release_ticket(struct lock* lock, volatile struct qnode* node)
{
        __add(&((lock->ticket_lock).tickets.head), 1, UNLOCK_LOCK_PREFIX);
}

static enum release_mode acquire_tas(struct lock* lock, volatile struct qnode* node)
{
#ifdef DELAY
        uint64_t delay = 1;
#endif
        enum release_mode mode = TAS;
        volatile uint8_t *l = &(lock->tas_lock);

        while (1) {
                if (UNLOCKED == *l) {
                        if (UNLOCKED == test_and_set(l))
                                return mode;
                        if (method_tuner == STATE_QUEUE)
                                mode = TAS_TO_QUEUE;
                        else if (method_tuner == STATE_TICKET)
                                mode = TAS_TO_TICKET;
                }
#ifdef DELAY
                spin_delay(delay);
                delay *= 2;
#endif
                if (MODE_QUEUE == lock->mode)
                        return acquire_queue(lock, node);
                else if (MODE_TICKET == lock->mode)
                        return acquire_ticket(lock, node);
        }
}

static enum release_mode acquire_tas_trylock(struct lock* lock, volatile struct qnode* node)
{
	volatile uint8_t *l = &(lock->tas_lock);

	if (UNLOCKED == *l) {
		if (UNLOCKED == test_and_set(l)) 
			return TAS;
		else return TRYLOCK_FAILED;			
	} else return TRYLOCK_FAILED;	
}

static void invalid_queue(struct lock* lock, volatile struct qnode* head);
static enum release_mode acquire_queue(struct lock* lock, volatile struct qnode* node)
{
        struct qnode* predecessor;

        node->next = NULL;
        predecessor = (struct qnode *)fetch_and_store(lock, (uint64_t) node);

        if (NULL == predecessor) {
                if (method_tuner == STATE_TAS)
                        return QUEUE_TO_TAS;
                else if (method_tuner == STATE_TICKET)
                        return QUEUE_TO_TICKET;
                else
                        return QUEUE;
        } else if ((struct qnode*) PNTINV != predecessor) {
                node->status = WAITING;
                predecessor->next = node;
                while (WAITING == node->status);

                if (GOT_LOCK == node->status)
                        return QUEUE;

                else if (MODE_TAS == lock->mode)
                        return acquire_tas(lock, node);
                else
                        return acquire_ticket(lock, node);
        } else {
                invalid_queue(lock, node);

                if (MODE_TAS == lock->mode)
                        return acquire_tas(lock, node);
                else
                        return acquire_ticket(lock, node);
        }
}

static enum release_mode acquire_queue_trylock(struct lock* lock, volatile struct qnode* node)
{
	int ret;
	
	node->next = NULL;
	ret = compare_and_swap(lock, (uint64_t)NULL, (uint64_t)node);
	if (1 == ret) return QUEUE;
	else return TRYLOCK_FAILED;		
}

static enum release_mode acquire_lock(struct lock* lock, volatile struct qnode* node)
{
        volatile uint8_t *l = &(lock->tas_lock);

        if (UNLOCKED == test_and_set(l))
                return TAS;
        else if (MODE_TAS == lock->mode)
                return acquire_tas(lock, node);
        else if (MODE_QUEUE == lock->mode)
                return acquire_queue(lock, node);
        else
                return acquire_ticket(lock, node);
}

static enum release_mode acquire_trylock(struct lock* lock, volatile struct qnode* node)
{

	if (MODE_TAS == lock->mode)
		return acquire_tas_trylock(lock, node);
	else if (MODE_QUEUE == lock->mode)
		return acquire_queue_trylock(lock, node);
	else 
		return acquire_ticket_trylock(lock, node);	
}

static void release_queue(struct lock* lock, volatile struct qnode* node)
{
        if (NULL == node->next) {
                if (compare_and_swap(lock, (uint64_t)node, (uint64_t)NULL))
                        return;

                while (NULL == node->next);
        }
        (node->next)->status = GOT_LOCK;
}

static void acquire_invalid_queue(struct lock *lock, volatile struct qnode* node)
{
        struct qnode* predecessor;

        while(1) {
                node->next = NULL;
                predecessor = (struct qnode*) fetch_and_store(lock, (uint64_t)node);
                if ((struct qnode*)PNTINV == predecessor) return;

                node->status = WAITING;
                predecessor->next = node;
                while (WAITING == node->status);
        }
}
static void acquire_invalid_ticket_queue(struct lock *lock, volatile struct qnode* node)
{
        register struct __reactive_raw_tickets inc = { .tail = 1 };
        inc = xadd(&((lock->ticket_lock).tickets), inc);

        if (inc.head != inc.tail)
                lock->ticket_lock.tickets.head = inc.tail;

        lock->ticket_lock.isvalid = true;
        barrier();
}

static inline void invalid_ticket_queue(struct lock* lock)
{
        lock->ticket_lock.isvalid = false;
}

static void invalid_queue(struct lock* lock, volatile struct qnode* head)
{
        struct qnode* tail = (struct qnode*)fetch_and_store(lock, (uint64_t)PNTINV);
        volatile struct qnode* next;

        while (head != tail) {
                while (NULL == head->next);
                next = head->next;
                head->status = INVALID;
                head = next;
        }

        head->status = INVALID;
}
static void release_lock(struct lock* lock, volatile struct qnode* node, enum release_mode mode)
{
        switch (mode) {
                case TAS:
                        COMPILER_BARRIER;
                        lock->tas_lock = UNLOCKED;
                        break;
                case QUEUE:
                        release_queue(lock, node);
                        break;
                case TICKET:
                        release_ticket(lock, node);
                        break;
                case TAS_TO_QUEUE:
                        acquire_invalid_queue(lock, node);
                        lock->mode = MODE_QUEUE;
                        release_queue(lock, node);
                        break;
                case QUEUE_TO_TAS:
                        COMPILER_BARRIER;
                        lock->mode = MODE_TAS;
                        invalid_queue(lock, node);
                        COMPILER_BARRIER;
                        lock->tas_lock = UNLOCKED;
                        break;
                case TAS_TO_TICKET:
                        acquire_invalid_ticket_queue(lock, node);
                        lock->mode = MODE_TICKET;
                        release_ticket(lock, node);
                        break;
                case QUEUE_TO_TICKET:
                        acquire_invalid_ticket_queue(lock, node);
                        COMPILER_BARRIER;
                        lock->mode = MODE_TICKET;
                        invalid_queue(lock, node);
                        release_ticket(lock, node);
                        break;
                case TICKET_TO_TAS:
                        COMPILER_BARRIER;
                        lock->mode = MODE_TAS;
                        invalid_ticket_queue(lock);
                        COMPILER_BARRIER;
                        lock->tas_lock = UNLOCKED;
                        break;
                case TICKET_TO_QUEUE:
                        invalid_ticket_queue(lock);
                        acquire_invalid_queue(lock, node);
                        lock->mode = MODE_QUEUE;
                        release_queue(lock, node);
                        break;
		case TRYLOCK_FAILED:
			printk("BUG: TRYLOCK_FAILED is passed\n");
			break;
        }
}

void __lockfunc _init_lock(struct lock* lock)
{
        lock->tas_lock = UNLOCKED;
        lock->queue_tail = (struct qnode*) PNTINV;
        lock->ticket_lock.tickets.head = 0;
        lock->ticket_lock.tickets.tail = 0;
        lock->ticket_lock.isvalid = false;
        lock->mode = MODE_TAS;
}
EXPORT_SYMBOL(_init_lock);

extern unsigned long total_lock_time;
extern unsigned long total_lock_cnt;
extern unsigned long call_read_tsc(void);

enum release_mode __lockfunc _acquire_mixed_lock(struct lock* lock, volatile struct qnode* node)
{
        unsigned long ts;
        enum release_mode ret;

        if (unlikely(0 == smp_processor_id())) {
                ts = call_read_tsc();
                ret = acquire_lock(lock, node);
                total_lock_time += call_read_tsc() - ts;
                total_lock_cnt++;
        } else
                ret = acquire_lock(lock, node);

        return ret;
}
EXPORT_SYMBOL(_acquire_mixed_lock);

enum release_mode __lockfunc _acquire_mixed_trylock(struct lock* lock, volatile struct qnode* node)
{
	return acquire_trylock(lock, node);
}
EXPORT_SYMBOL(_acquire_mixed_trylock);

void __lockfunc _release_mixed_lock(struct lock* lock, volatile struct qnode* node, enum release_mode mode)
{
        release_lock(lock, node, mode);
}
EXPORT_SYMBOL(_release_mixed_lock);
