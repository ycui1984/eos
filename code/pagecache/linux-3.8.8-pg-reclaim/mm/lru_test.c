#ifndef CONFIG_RECLAIM_POLICY
/*
 * Implements the LRU page replacement class - based on the Least Recently Used
 * page replacement algorithm.
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/buffer_head.h>	/* for try_to_release_page(),
					buffer_heads_over_limit */
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/compaction.h>

#include "internal.h"

#define DEBUG		1

/* Policy functions. */
static void init_lruvec_lru(struct lruvec *lruvec)
{
	/*enum lru_list lru;

	memset(lruvec, 0, sizeof(struct lruvec));

	for_each_lru(lru)
		INIT_LIST_HEAD(&lruvec->lists[lru]);
	*/
	printk("LRU: init_lruvec()\n");
}

/*
 * Reclaim/compaction is used for high-order allocation requests. It reclaims
 * order-0 pages before compacting the zone. should_continue_reclaim() returns
 * true if more pages should be reclaimed such that when the page allocator
 * calls try_to_compact_zone() that it will have enough free pages to succeed.
 * It will give up earlier than that if there is difficulty reclaiming pages.
 */
static inline bool should_continue_reclaim_lru(struct lruvec *lruvec,
					unsigned long nr_reclaimed,
					unsigned long nr_scanned,
					struct scan_control *sc)
{
	printk("LRU:should_continue_reclaim().\n");
	return false;
}

static int shrink_lruvec_lru(struct lruvec *lruvec, struct scan_control *sc,
			     unsigned long *nr)
{
	printk("LRU: shrink_lruvec()\n");
	return 1;
}

static void balance_lruvec_lru(struct lruvec *lruvec, struct scan_control *sc)
{
	printk("LRU:balance_lruvec_lru().\n");
}

static void get_scan_count_lru(struct lruvec *lruvec, struct scan_control *sc,
			       unsigned long *nr, bool force_scan)
{
	printk("LRU:get_scan_count().\n");
}

static void add_page_to_list_lru(struct page *page, struct lruvec *lruvec,
				 int list_index)
{
	//printk("LRU: add_page_to_list()\n");
}

static void del_page_from_list_lru(struct page *page, struct lruvec *lruvec,
				   int list_index)
{
	//printk("LRU: del_page_from_list()\n");
}

static bool too_many_isolated_comapction_lru(struct zone *zone)
{
	printk("LRU:too_many_isolated_compaction_lru().\n");
	return false;
}

/*
 * The reclaimable count would be mostly accurate.
 * The less reclaimable pages may be
 * - mlocked pages, which will be moved to unevictable list when encountered
 * - mapped pages, which may require several travels to be reclaimed
 * - dirty pages, which is not "instantly" reclaimable
 */
unsigned long global_reclaimable_pages_lru(void)
{
	printk("LRU:global_reclaimable_pages().\n");
	return 100;
}

unsigned long zone_reclaimable_pages_lru(struct zone *zone)
{
	printk("LRU:zone_reclaimable_pages().\n");
	return 100;
}


const struct page_replacement_class lru_page_replacement_class =
{
	.init_lruvec = init_lruvec_lru, /* init */
	.shrink_lruvec = shrink_lruvec_lru,/* shrink the lruvec */
	.balance_lruvec = balance_lruvec_lru, /* balance the lruvec */
	.should_continue_reclaim = should_continue_reclaim_lru,
	.add_page_to_list = add_page_to_list_lru, /* literal add */
	.del_page_from_list = del_page_from_list_lru, /* literal delete */
	.zone_reclaimable_pages = zone_reclaimable_pages_lru,
	.global_reclaimable_pages = global_reclaimable_pages_lru,
	.get_scan_count = get_scan_count_lru, /* how many pages to scan */
	.too_many_isolated_compaction = too_many_isolated_comapction_lru,
};

#endif /* CONFIG_RECLAIM_POLICY */
