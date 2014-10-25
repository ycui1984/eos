#include <linux/module.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/timer.h>
#include <linux/netdevice.h>

MODULE_AUTHOR("agui");
MODULE_DESCRIPTION("kernel thread example");
MODULE_LICENSE("GPL");

static struct task_struct * agui_task;

static int agui_thread(void * nothing)
{
	//set_freezable();

        //allow_signal(SIGKILL);

        while(!kthread_should_stop()) {
                set_current_state(TASK_INTERRUPTIBLE);
                schedule_timeout(msecs_to_jiffies(6000));
		printk("10 mins passed\n");
        }

        printk("kernel thread exit\n");
        return 0;
}

static void net_init(void) 
{
	struct net_device *dev;
	struct rtnl_link_stats64 temp, *stats;
	
	dev = first_net_device_rcu(&init_net);
	while (dev) {
		if (0 != strcmp("lo", dev->name)) {
			printk(KERN_INFO "found [%s]\n", dev->name);
			stats = dev_get_stats(dev, &temp);
			printk(KERN_INFO "%lu\t%lu\t%lu\t%lu\n", stats->rx_bytes, stats->rx_packets,
			stats->tx_bytes, stats->tx_packets);		
		}
		dev = next_net_device_rcu(dev);		
	}
	
}

static int fast_convergence = 1;
static int max_increment = 2;

module_param(fast_convergence, int, 0644);
module_param(max_increment, int, 0644);


static int __init thread_init(void)
{
//	struct proc_dir_entry *entry;
        int error;

/*
	entry = create_proc_entry("tcp_auto_test", 0600, init_net.proc_net);
	if (!entry) 
		return -ENOMEM;
	

	int a;
	a = 2;

	typedef struct a {
	int a	
	} A;
*/
//	module_param(fast_convergence, int, 0644);
//	module_param(max_increment, int, 0644);
	 

//	entry->write_proc = proc_write;
//	net_init();
        agui_task = kthread_run(agui_thread, NULL, "ExampleThread");
        if(IS_ERR(agui_task))
        {
                error = PTR_ERR(agui_task);
                return error;
        }
	
        return 0;
}

static void __exit thread_exit(void)
{
	//remove_proc_entry("/proc/sys/net/tcp_auto", init_net.proc_net);	
        kthread_stop(agui_task);
}

module_init(thread_init);
module_exit(thread_exit);

