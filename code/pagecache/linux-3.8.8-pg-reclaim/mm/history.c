#include <linux/mm_types.h>
#include <linux/mmzone.h>
#include <linux/page-flags.h>
#include <linux/nonresident.h>
#include <linux/mm.h>

void add_to_history(struct page *page, struct list_head *history, int index)
{
	if (!&page->history) {
		INIT_LIST_HEAD(&page->history);
	}
	list_add(&page->history, &history[index]);
}

void delete_from_history(struct page *page, struct list_head *history, int index)
{
	if (&page->history) {
		list_del(&page->history);
	}
}

/* Move the page in the history list to the top of the referenced pages. */
void rotate_page_history(struct page *page, struct list_head *history, int index)
{
	if (&page->history) {
		/* This is causing the kernel to crash. For now, we don't
		 * account when a page has been accessed which might influence
		 * when the cache is re-warmed.*/

		/* list_del(&page->history);
		list_add(&page->history, &history[index]);
		*/
	}
}

int get_history_index(enum lru_list lru)
{
	return lru == LRU_UNEVICTABLE ? HISTORY_UNEVICTABLE : HISTORY_EVICTABLE;
}

int switch_reclaim_policy(struct zone *zone, enum pgreclaim_policy_list old_p,
			  enum pgreclaim_policy_list new_p)
{
	struct page *page;
	struct lruvec *lruvec;
	struct list_head *history_list;
	int lru, active;
	unsigned long nr_evictable=0;
	const struct page_reclaim_policy *old_policy, *new_policy;

	if (old_p == new_p) {
		printk("switch_reclaim_policy: cannot switch to the same policy.\n");
		return 0;
	}
	old_policy = reclaim_policy(old_p);
	new_policy = reclaim_policy(new_p);
	lruvec = zone->lruvecs[old_p];

	spin_lock_irq(&zone->lru_lock);

	zone->policy = new_p;
	//move evictable pages
	old_policy->reset_zone_vmstat(lruvec, zone, 1);
	
	history_list = &zone->history[HISTORY_EVICTABLE];
	list_for_each_entry_reverse(page, history_list, history) {
		if(!PageUnevictable(page) && PageLRU(page) && (&page->history) ) {
		lru = LRU_INACTIVE_ANON;
//		lru = PageSwapBacked(page) ? LRU_INACTIVE_ANON :
//						     LRU_INACTIVE_FILE;

		active = PageActive(page);	
			
//		printk(KERN_ERR "before list del page %d, active %d\n", new_p, active);
		list_del(&page->lru);
//		printk(KERN_ERR "before add page %ld\n", nr_evictable);
		
		new_policy->add_page(page, zone->lruvecs[new_p],
				     lru + active);
		
		nr_evictable ++;
		}
	}
	
	printk(KERN_ERR "finish move evictable pages, %ld\n", nr_evictable);
	//move unevictable pages
	old_policy->reset_zone_vmstat(lruvec, zone, 0);
	history_list = &zone->history[HISTORY_UNEVICTABLE];
	list_for_each_entry_reverse(page, history_list, history) {
		list_del(&page->lru);
//		new_policy->add_page(page, zone->lruvecs[new_p],
//				     LRU_UNEVICTABLE);
		new_policy->add_page_unevictable(page, zone->lruvecs[new_p]);
	}

	nonresident_set_policy(new_p);

	printk(KERN_ERR "finish move nonevictable pages %d\n", nonresident_get_policy());

	spin_unlock_irq(&zone->lru_lock);
	return 0;
}

int switch_reclaim_policy_all(enum pgreclaim_policy_list old_p,
			      enum pgreclaim_policy_list new_p)
{
	struct zone *zone;

	if (old_p == new_p) {
		printk("switch_reclaim_policy_all: cannot switch to the same policy.\n");
		return 0;
	}

	for_each_zone(zone) {
		switch_reclaim_policy(zone, old_p, new_p);
	}

	return 0;
}


