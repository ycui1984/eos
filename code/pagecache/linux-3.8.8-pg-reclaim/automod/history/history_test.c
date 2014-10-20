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
#define PRA_DESC   "Module to test the history lists of the zones."

//Start - get memory nodes and zones
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
//End - get memory nodes and zones

const struct page_reclaim_policy *get_policy(enum pgreclaim_policy_list p) {
	switch (p) {
		case PAGE_RECLAIM_FIFO:
			return &fifo_page_reclaim_policy;
		default:
			return &lru_page_reclaim_policy;
	}
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

static void assert_equal(int a, int b)
{
	if (a != b) {
		printk("Assert failed. %d is not equal to %d.\n", a, b);
	} else {
		printk("Assert passed. %d\n", a);
	}
}

static void test_lru_list_count(void)
{
	struct zone *zone;
	int count = 0;
	struct list_head *list;
	int lru, lru_count, history_count, t;
	const struct page_reclaim_policy *policy;

	printk("LRU2Q: test count *********************\n");
	for_each_zone(zone) {
		count++;
		printk("ZONE_%s-%d policy:%d.\n", zone->name, count, zone->policy);

		printk("EVICTABLE pages\n");
		history_count = get_count(&zone->history[HISTORY_EVICTABLE]);
		printk("History: %d; ", history_count);

		policy = get_policy(PAGE_RECLAIM_LRU2Q);
		lru_count = 0;
		for (lru = 0; lru < NR_LRU_LISTS - 1; lru++) {
			list = policy->get_list(zone->lruvecs[PAGE_RECLAIM_LRU2Q], lru);
			t = get_count(list);
			lru_count += t;
			printk(" L-%d: %d;", lru, t);
		}
		printk("\n");
		assert_equal(lru_count, history_count);


		printk("UNEVICTABLE pages\n");
		history_count = get_count(&zone->history[HISTORY_UNEVICTABLE]);
		list = policy->get_list(zone->lruvecs[PAGE_RECLAIM_LRU2Q], LRU_UNEVICTABLE);
		lru_count = get_count(list);
		printk("History: %d; List:%d;\n", history_count, lru_count);
		assert_equal(lru_count, history_count);

		printk("\n");
	}
}

static void test_fifo_list_count(void)
{
	struct zone *zone;
	int count = 0;
	struct list_head *list;
	int lru, fifo_count, history_count, t;
	const struct page_reclaim_policy *policy;

	printk("FIFO: test count *********************\n");
	for_each_zone(zone) {
		count++;
		printk("ZONE_%s-%d policy:%d.\n", zone->name, count, zone->policy);

		printk("EVICTABLE pages\n");
		history_count = get_count(&zone->history[HISTORY_EVICTABLE]);
		policy = get_policy(PAGE_RECLAIM_FIFO);
		fifo_count = 0;

		lru = 0; //only FIFO_EVICTABLE
		list = policy->get_list(zone->lruvecs[PAGE_RECLAIM_FIFO], lru);
		t = get_count(list);
		fifo_count += t;
		printk("History: %d; List:%d;\n", history_count, fifo_count);
		assert_equal(fifo_count, history_count);

		printk("UNEVICTABLE pages\n");
		lru = 1; //only FIFO_UNEVICTABLE
		history_count = get_count(&zone->history[HISTORY_UNEVICTABLE]);
		list = policy->get_list(zone->lruvecs[PAGE_RECLAIM_FIFO], lru);
		fifo_count = get_count(list);
		printk("History: %d; List:%d;\n", history_count, fifo_count);
		assert_equal(fifo_count, history_count);

		printk("\n");
	}
}

static __init int history_init(void)
{
	int ret = 0;
	printk(KERN_INFO "init(): history_test module.\n");
	test_lru_list_count();
	test_fifo_list_count();
	return ret;
}

static __exit void history_exit(void)
{
	printk(KERN_INFO "exit(): history_test module.\n\n");
	return;
}

MODULE_AUTHOR(PRA_AUTHOR);
MODULE_LICENSE(PRA_LICENSE);
MODULE_DESCRIPTION(PRA_DESC);

module_init(history_init);
module_exit(history_exit);
