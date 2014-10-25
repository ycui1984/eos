#include <linux/module.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/timer.h>
#include <linux/mutex.h>
#include <linux/freezer.h>

#define MAXSTUFF        32
#define MAXCORES        8
#define DEBUG

atomic_t flag;
static int the_time = 60000;
static int lock_cycles = 100000;
static int unlock_cycles = 0;

static int nworkers = 4;
module_param(nworkers, int, 0644);
MODULE_PARM_DESC(nworkers, "the number of workers");

module_param(the_time, int, 0644);
module_param(lock_cycles, int, 0644);
module_param(unlock_cycles, int, 0644);

struct percore_stuff {
	atomic_t count;
	char pad[0] __attribute__((aligned(64)));
};

static struct percore_stuff stuff[MAXCORES];
static DEFINE_MUTEX(mlock);
 
static struct task_struct *tasks[MAXSTUFF];
static struct task_struct *master;

static inline unsigned long read_tsc(void)
{
	unsigned int low, high;
	__asm__ __volatile__("rdtsc" : "=a" (low),"=d" (high));
	
	return (unsigned long)high << 32 | low;
}

static inline void delay(uint64_t cycles)
{
        uint64_t s = read_tsc();
        while ((read_tsc() - s ) < cycles)
                cpu_relax();
}

static void collect_stats(void)
{
	int i;
	unsigned int tot;
	int sec, tput;

#ifdef DEBUG
	printk("%s\n", __FUNCTION__);
#endif
		
	sec = the_time / 1000.0;
	
	tot = 0;
	for (i = 0; i < MAXCORES; i++) {
#ifdef DEBUG
		printk("%d ", atomic_read(&stuff[i].count));
#endif
		tot += atomic_read(&stuff[i].count);
	}

#ifdef DEBUG
	printk("\n");
#endif

	tput = tot / sec;
	printk("%d\n", tput);

	return;
}

static void worker(void *);

static int manager(void *x) 
{
	int i, errno;

#ifdef DEBUG
	printk("%s\n", __FUNCTION__);
#endif
        for (i = 0; i < nworkers; i++) {
                tasks[i] = kthread_run(worker, (void *)i, "worker");
                if(IS_ERR(tasks[i])) {
                        errno = PTR_ERR(tasks[i]);
			tasks[i] = NULL;
                 
			return errno;
                }

        }
	
	atomic_set(&flag, 1);
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(msecs_to_jiffies(the_time));
	collect_stats();

        for (i = 0; i < nworkers; i++)
	        kthread_stop(tasks[i]);
	
	return 0;
}

static void worker(void *x) {
	
	unsigned int id;
	struct percore_stuff *s;
	
	id = (long)x;

#ifdef DEBUG
	printk("work NO%d\n", id);
#endif

	s = &stuff[id % MAXCORES];	

	while (1 != atomic_read(&flag));

	while (!kthread_should_stop()) {
		delay(unlock_cycles);
		mutex_lock(&mlock);
#ifdef DEBUG
		printk("%d\n", current->pid);	
#endif
		delay(lock_cycles);
		mutex_unlock(&mlock);
		atomic_inc(&s->count);
	}	
}

static int __init bench_init(void)
{
	int i, errno;

	for (i = 0; i < MAXSTUFF; i++) 
		tasks[i] = NULL;
	
	for (i = 0; i < MAXCORES; i++) 
		atomic_set(&stuff[i].count, 0);			

	atomic_set(&flag, 0);

	master = kthread_run(&manager, NULL, "master");
	if (IS_ERR(master)) {
		errno = PTR_ERR(master);
		master = NULL;
		return errno;
	}
		
	return 0;
}

static void __exit bench_exit(void)
{
#ifdef DEBUG
	printk("exit benchmarking\n");
#endif
}

MODULE_AUTHOR("Yan Cui <ccuiyyan@gmail.com>");
MODULE_LICENSE("GPL");
module_init(bench_init);
module_exit(bench_exit);

