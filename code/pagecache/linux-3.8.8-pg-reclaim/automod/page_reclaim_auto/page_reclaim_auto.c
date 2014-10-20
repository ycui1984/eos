#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <asm/uaccess.h>
#include <linux/genhd.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>

#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/mmzone.h>

#define PRA_AUTHOR "Petre Lukarov <pbl2108@columbia.edu>"
#define PRA_LICENSE "GPL"
#define PRA_DESC   "Sample tests for the Page Reclaim Algorithm"

static struct file *open_file(char const *file_name, int flags, int mode)
{
	struct file *file = NULL;
#if BITS_PER_LONG != 32
	flags |= O_LARGEFILE;
#endif
	file = filp_open(file_name, flags, mode);

	return file;
}

static void close_file(struct file *file)
{
	if (file->f_op && file->f_op->flush) {
		file->f_op->flush(file, current->files);
	}
	fput(file);
}

static int kernel_write(struct file *file, unsigned long offset,
			const char *addr, unsigned long count)
{
	mm_segment_t old_fs;
	loff_t pos = offset;
	int result = -ENOSYS;

	if (!file->f_op->write)
		goto fail;
	old_fs = get_fs();
	set_fs(get_ds());
	result = file->f_op->write(file, addr, count, &pos);
	set_fs(old_fs);
fail:
	return result;
}

/*static int echo_to_procfs(char degree)
{
	struct file *file;
	int ret;

	file = open_file("/proc/sys/vm/drop_caches", O_RDWR, 0777);

	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		printk("cannot open /proc/sys/vm/drop_caches to write, error=%d\n", ret);
		return -1;
	}	

	ret = kernel_write(file, 0, &degree, 1);
	if (ret < 0) {
		printk("cannot write to /proc/sys/vm/drop_caches\n");
		return -1;
	}

	close_file(file);

	return 0;
}*/

static int echo_to_log(char *buf, int count) {
	struct file *file;
	int ret;

	file = open_file("/home/peter/repos/linux-stable/p.log", O_RDWR, 0777);

	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		printk("cannot open 'p.log' to write, error=%d\n", ret);
		return -1;
	}	

	ret = kernel_write(file, 0, buf, count);
	if (ret < 0) {
		printk("cannot write to 'p.log'\n");
		return -1;
	}

	close_file(file);

	return 0;
}

/*static char *read_into_buffer(char *file_name, int *size)
{
	int ret, file_size, data_read = 0;
	struct file *file;
	void *buffer;
	
	file = open_file(file_name, O_RDONLY, 00777);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		printk("Cannot open %s for reading, error = %d\n", file_name,
			ret);
		return NULL;
	}
	
	file_size = file->f_mapping->host->i_size;
	if (file_size <=0) {
		printk("Empty file\n");
		return NULL;
	}	
	
	buffer = alloc_buffer(file_size + 1);
	memset(buffer, '\0', file_size + 1);
	
	if (NULL == buffer) {
		printk("Cannot allocate memory\n");
		close_file(file);
		return NULL;
	}	
	
	while (data_read < file_size) {
		
		ret = kernel_read(file, data_read, buffer + data_read,
				  file_size - data_read);
			
		if (ret < 0) {
			printk("Error in reading file, error = %d\n", ret);
			goto out_close_free;
		} else if (0 == ret) {
			printk("File is too small\n");
			break;
		}
		data_read += ret;
	}
	close_file(file);
	*size = file_size + 1;

	return buffer;

out_close_free:
	close_file(file);
	free_buffer(buffer, file_size + 1);

	return NULL;
}*/


struct car {
	int reg_number;
	struct list_head cars_list;
};

static void print_nodes_using_prev(struct list_head *list) {
	struct car *c;
	while (!list_empty(list)) {
		c = list_entry(list->prev, struct car, cars_list);
		printk(KERN_INFO "prev():Car %d.\n", c->reg_number);
		list_del(&c->cars_list);
		kfree(c);
	}
}

static void print_nodes_using_next(struct list_head *list) {
	struct car *c;
	c = list_entry(list->next, struct car, cars_list);
	printk(KERN_INFO "NEXT:Car %d.\n", c->reg_number);
	c = list_entry(list->prev, struct car, cars_list);
	printk(KERN_INFO "PREV:Car %d.\n", c->reg_number);
	while (!list_empty(list)) {
		c = list_entry(list->next, struct car, cars_list);
		printk(KERN_INFO "next():Car %d.\n", c->reg_number);
		list_del(&c->cars_list);
		kfree(c);
	}
}

//uses list_add() and print_prev
static void test1(void) {
	LIST_HEAD(car_list1);
	LIST_HEAD(car_list2);
	struct car *c;

	printk(KERN_INFO "test1():list_add() and print_prev********\n");
	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 1;
	INIT_LIST_HEAD(&c->cars_list);
	list_add(&(c->cars_list), &car_list1);

	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 2;
	INIT_LIST_HEAD(&c->cars_list);
	list_add(&(c->cars_list), &car_list1);

	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 3;
	INIT_LIST_HEAD(&c->cars_list);
	list_add(&(c->cars_list), &car_list1);

	print_nodes_using_prev(&car_list1);
}

//uses list_add_tail() and print_prev
static void test2(void) {
	LIST_HEAD(car_list1);
	LIST_HEAD(car_list2);
	struct car *c;

	printk(KERN_INFO "test2():list_add_tail() and print_prev******\n");
	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 1;
	INIT_LIST_HEAD(&c->cars_list);
	list_add_tail(&(c->cars_list), &car_list1);

	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 2;
	INIT_LIST_HEAD(&c->cars_list);
	list_add_tail(&(c->cars_list), &car_list1);

	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 3;
	INIT_LIST_HEAD(&c->cars_list);
	list_add_tail(&(c->cars_list), &car_list1);

	print_nodes_using_prev(&car_list1);
}

//uses list_add() and print_next
static void test3(void) {
	LIST_HEAD(car_list1);
	LIST_HEAD(car_list2);
	struct car *c, *t;
	struct list_head *pos;

	printk(KERN_INFO "test3():list_add() and print_next******\n");
	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 10;
	INIT_LIST_HEAD(&c->cars_list);
	list_add(&(c->cars_list), &car_list1);

	t = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	t->reg_number = 20;
	INIT_LIST_HEAD(&t->cars_list);
	list_add(&(t->cars_list), &car_list1);

	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 30;
	INIT_LIST_HEAD(&c->cars_list);
	list_add(&(c->cars_list), &car_list1);

	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 40;
	INIT_LIST_HEAD(&c->cars_list);
	list_add(&(c->cars_list), &car_list1);

	//print_nodes_using_next(&car_list1);

	list_del(&t->cars_list);
	list_add(&t->cars_list, &car_list1);

	list_for_each(pos, &car_list1) {
		c = list_entry(pos, struct car, cars_list);
		printk("Entry:%d\n", c->reg_number);
	}
}

//uses list_add_tail() and print_next
static void test4(void) {
	LIST_HEAD(car_list1);
	LIST_HEAD(car_list2);
	struct car *c;

	printk(KERN_INFO "test4():list_add_tail() and print_next******\n");
	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 10;
	INIT_LIST_HEAD(&c->cars_list);
	list_add_tail(&(c->cars_list), &car_list1);

	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 20;
	INIT_LIST_HEAD(&c->cars_list);
	list_add_tail(&(c->cars_list), &car_list1);

	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 30;
	INIT_LIST_HEAD(&c->cars_list);
	list_add_tail(&(c->cars_list), &car_list1);

	c = (struct car*)kmalloc(sizeof(*c), GFP_KERNEL);
	c->reg_number = 40;
	INIT_LIST_HEAD(&c->cars_list);
	list_add_tail(&(c->cars_list), &car_list1);

	print_nodes_using_next(&car_list1);
}

static void test5(void) {
	char *c = "Hello World!";
	echo_to_log(c, 10);
}

static void dummy_tests(void) {
	test1();
	test2();
	test3();
	test4();
	test5();
}

static int myint = 1024;
module_param(myint, int, 0);

struct pglist_data *first_online_pgdat(void)
{
	return NODE_DATA(first_online_node);
}

struct pglist_data *next_online_pgdat(struct pglist_data *pgdat)
{
	int nid = next_online_node(pgdat->node_id);

	if (nid == MAX_NUMNODES)
		return NULL;
	return NODE_DATA(nid);
}

/*
 * next_zone - helper magic for for_each_zone()
 */
struct zone *next_zone(struct zone *zone)
{
	pg_data_t *pgdat = zone->zone_pgdat;

	if (zone < pgdat->node_zones + MAX_NR_ZONES - 1)
		zone++;
	else {
		pgdat = next_online_pgdat(pgdat);
		if (pgdat)
			zone = pgdat->node_zones;
		else
			zone = NULL;
	}
	return zone;
}

void test_correct_zones(void) {
	struct zone *zone;
	int count = 0;
	bool correct = false;
	printk("MAX_NR_ZONES: %d\n", MAX_NR_ZONES);
	for_each_zone(zone) {
		count++;
		if (zone) {
			correct = zone->lruvecs[zone->policy] != NULL;
			printk("Zone-%d Correct? - %c.\n", count,
			       (correct ? 'Y' : 'N'));
		}
	}
}

const struct page_reclaim_policy *get_policy(enum pgreclaim_policy_list p) {
	switch (p) {
		case PAGE_RECLAIM_FIFO:
			return &fifo_page_reclaim_policy;
		default:
			return &lru_page_reclaim_policy;
	}
}

void test_lruvec_opaque(void) {
	struct zone *zone;
	const struct page_reclaim_policy *policy;
	int count = 0;
	enum pgreclaim_policy_list i;
	bool correct = false;
	printk("MAX_NR_ZONES: %d\n", MAX_NR_ZONES);
	for_each_zone(zone) {
		count++;
		if (zone) {
			for (i = PGRECLAIM_BASE; i < NR_PAGE_RECLAIM_POLICIES; i++) {
				policy = get_policy(i);
				policy->print_lruvec(zone);
				printk("\n");

				correct = zone->lruvecs[i] != NULL;
				printk("Zone-%d lruvec[%d]:Correct? - %c.\n",
					count,
					i,
					(correct ? 'Y' : 'N'));
			}
		}
	}
}

static int output_number_of_pages(void) {
	struct zone *zone;
	struct pglist_data *pgdat;
	int count = 0;
	int count_nodes = 0;
	unsigned long start_pfn, end_pfn;

	for_each_online_pgdat(pgdat) {
		start_pfn = pgdat->node_start_pfn;
		end_pfn = pgdat->node_start_pfn + pgdat->node_spanned_pages;
		count_nodes++;
		printk("Node-%d, start_pfn:%lu, end_pfn:%lu\n", count_nodes,
			start_pfn, end_pfn);
	}

	printk("MAX_NR_ZONES: %d\n", MAX_NR_ZONES);
	for_each_zone(zone) {
		count++;
		//printk("Zone-%d wmark: min:%lu low:%lu high:%lu\n", count,
		//       min_wmark_pages(zone),
		//       low_wmark_pages(zone),
		//       high_wmark_pages(zone));
		start_pfn = zone->zone_start_pfn;
		end_pfn = zone->zone_start_pfn + zone->spanned_pages;
		printk("Zone-%d start_pfn:%lu end_pfn:%lu\n", count, start_pfn,
			end_pfn);
		//zone->page_reclaim_policy->print_lruvec_p(zone->lruvec_p);
	}
	printk("Number of zones on the computer: %d.\n", count);
	return 0;
}

static int get_count(struct list_head *list)
{
	struct list_head *pos;
	int count = 0;
	list_for_each(pos, list) {
		count++;
	}
	return count;
}

static void test_each_zone_print(void)
{
	struct zone *zone;
	int count = 0;
	struct list_head *list;
	int lru;
	const struct page_reclaim_policy *policy;

	for_each_zone(zone) {
		count++;
		printk("Zone-%d policy:%d.\n", count, zone->policy);
		policy = get_policy(zone->policy);
		for_each_lru(lru) {
			list = policy->get_list(zone->lruvecs[zone->policy], lru);
			printk("List-%d count elements:%d.\n", lru, get_count(list));
		}
	}
}

static __init int auto_init(void)
{
	int ret = 0;
	printk(KERN_INFO "init(): Page reclaim module init.\n");
	printk(KERN_INFO "init(): myint is an integer: %d\n", myint);
	test3();
	//output_number_of_pages();
	//test_correct_zones();
	//test_lruvec_opaque();
	//test_each_zone_print();
	return ret;
}

static __exit void auto_exit(void)
{
	printk(KERN_INFO "exit():****************************************\n");
	return;
}

MODULE_AUTHOR(PRA_AUTHOR);
MODULE_LICENSE(PRA_LICENSE);
MODULE_DESCRIPTION(PRA_DESC);

module_init(auto_init);
module_exit(auto_exit);
