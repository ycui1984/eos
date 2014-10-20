#ifdef CONFIG_RECLAIM_POLICY
#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/mmzone.h>

asmlinkage long sys_test_page_replacement_class(int i) {
	//const struct page_replacement_class *p;
	//printk(KERN_DEBUG "Hello World! The number was %d\n", i);
	//p = &lru_page_replacement_class;
	//p->pick_next_page(12);
	return(0);
}

/*SYSCALL_DEFINE1(test_page_replacement_class, int, i)
{
	printk(KERN_DEBUG "Hello World! The number was %dn", i);
	int retval = 0;
	return retval;
}*/
#endif
