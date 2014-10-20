#include <linux/module.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/timer.h>


MODULE_AUTHOR("Yan Cui");
MODULE_DESCRIPTION("lock latency test");
MODULE_LICENSE("GPL");

#define MC 	8

static const int affinity[] = {
	0, 1, 2, 3, 
	4, 5, 6, 7
};

static union {
    struct lock test_lock;
    char __pad[0] __attribute__((aligned(64)));
} g_lock;

static volatile struct {
    union {
        struct {
	    volatile uint64_t ops;
	    volatile uint64_t running;
	    volatile uint64_t cycles;
        };
	char __pad[0] __attribute__((aligned(64)));
    } cores[MC];
    volatile int state;
} g_state;

enum { CREATED = 1, READY, START, END };
enum { iterations = 100000 };

static void clean_warm()
{
    do_lock(&g_lock.lock);
    do_unlock(&g_lock.lock);
}

static void* worker_thread(void *arg)
{
    int cid = (uint64_t)arg;
    if (cid)
        affinity_set(affinity[cid]);
    clean_warm(1);
    if (cid) {
        g_state.state = READY;
        while (g_state.state != START)
	    nop_pause();
    }
    else
	g_state.state = START;
    uint64_t ops = 0;
    uint64_t s = read_tsc();
    while (g_state.state != END) {
        do_lock(&g_lock.lock);
	do_unlock(&g_lock.lock);
        ops ++;
	if (!cid && ops == iterations)
	    g_state.state = END;
    }
    uint64_t e = read_tsc();
    g_state.cores[cid].ops = ops;
    g_state.cores[cid].cycles = e - s;
    g_state.cores[cid].running = 0;
    return 0;
}

static void ontimer(int sig)
{
    g_state.state = END;
}

static int __init thread_init(void)
{
	/*
    	if (setpriority(PRIO_PROCESS, 0, -20) < 0)
		printk("unable to set process priority\n");
    	affinity_set(affinity[0]);
	*/
    	int ncores = MC;
    	memset((void *)&g_state, 0, sizeof(g_state));

    	struct itimerval tv;
    	tv.it_interval.tv_sec  = 5;
    	tv.it_interval.tv_usec = 0;
    	tv.it_value = tv.it_interval;
    	errno_check(setitimer(ITIMER_REAL, &tv, NULL));
    	signal(SIGALRM, ontimer);

    	lock_init(&g_lock.lock);

    	for (int i = 1; i < ncores; i++) {
		g_state.state = CREATED;
		g_state.cores[i].running = 1;
		pthread_start(worker_thread, i);
		while (g_state.state != READY)
		    	nop_pause();
    	}
    	worker_thread(0);

    	uint64_t tot_cpu_cycles = 0;
    	uint64_t tot_ops = 0;
    	for (int i = 0; i < JOS_NCPU; i++) {
        	while (g_state.cores[i].running)
	    		nop_pause();
        	tot_ops += g_state.cores[i].ops;
		tot_cpu_cycles += g_state.cores[i].cycles;
    	}
    	double ns = tot_cpu_cycles * (double)1000000000 / (tot_ops * get_cpu_freq());
    	printf("%d\t%.0f\n", ncores, ns / ncores);
    	return 0;
}

static void __exit thread_exit(void)
{
    
}

module_init(thread_init);
module_exit(thread_exit);

