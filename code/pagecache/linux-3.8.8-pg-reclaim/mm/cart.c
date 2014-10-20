/*
 * mm/cart.c
 *
 * Written by Peter Zijlstra <a.p.zijlstra@chello.nl>
 * Released under the GPLv2, see the file COPYING for details.
 *
 * This file contains a Page Replacement Algorithm based on CART
 * Please refer to the CART paper here -
 *   http://www.almaden.ibm.com/cs/people/dmodha/clockfast.pdf
 *
 * T1 -> active_list     |T1| -> nr_active
 * T2 -> inactive_list   |T2| -> nr_inactive
 * filter bit -> PG_longterm
 *
 * The algorithm was adapted to work for linux which poses the following
 * extra constraints:
 *  - multiple memory zones,
 *  - fault before reference,
 *  - expensive refernce check.
 *
 * The multiple memory zones are handled by decoupling the T lists from the
 * B lists, keeping T lists per zone while having global B lists. See
 * mm/nonresident.c for the B list implementation. List sizes are scaled on
 * comparison.
 *
 * The paper seems to assume we insert after/on the first reference, we
 * actually insert before the first reference. In order to give 'S' pages
 * a chance we will not mark them 'L' on their first cycle (PG_new).
 *
 * Also for efficiency's sake the replace operation is batched. This to
 * avoid holding the much contended zone->lru_lock while calling the
 * possibly slow page_referenced().
 *
 * All functions that are prefixed with '__' assume that zone->lru_lock is taken.
 */

#include <linux/rmap.h>
#include <linux/buffer_head.h>
#include <linux/pagevec.h>
#include <linux/bootmem.h>
#include <linux/init.h>
#include <linux/nonresident.h>
#include <linux/swap.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/writeback.h>

#include <asm/div64.h>

#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/buffer_head.h>
#include <linux/mm_inline.h>

#include "internal.h"

#include <linux/backing-dev.h>

//#include <linux/mm_cart_policy.h>
//#include <linux/mm_cart_data.h>
//#include <linux/mm_page_replace.h>

#define lru_to_page_cart(_head) (list_entry((_head)->prev, struct page, lru))

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page_cart(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page_cart(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif


#define CART_RECLAIMED_T1	0
#define CART_SATURATED		1

#define ZoneReclaimedT1(z)	test_bit(CART_RECLAIMED_T1, &((z)->flags))
#define SetZoneReclaimedT1(z)	__set_bit(CART_RECLAIMED_T1, &((z)->flags))
#define ClearZoneReclaimedT1(z)	__clear_bit(CART_RECLAIMED_T1, &((z)->flags))

#define ZoneSaturated(z)	test_bit(CART_SATURATED, &((z)->flags))
#define SetZoneSaturated(z)	__set_bit(CART_SATURATED, &((z)->flags))
#define TestClearZoneSaturated(z)  __test_and_clear_bit(CART_SATURATED, &((z)->flags))


#define PG_t1		PG_reclaim1
#define PG_longterm	PG_reclaim2
#define PG_new		PG_reclaim3

#define PageT1(page)		test_bit(PG_t1, &(page)->flags)
#define SetPageT1(page)		set_bit(PG_t1, &(page)->flags)
#define ClearPageT1(page)	clear_bit(PG_t1, &(page)->flags)
#define TestClearPageT1(page)	test_and_clear_bit(PG_t1, &(page)->flags)
#define TestSetPageT1(page)	test_and_set_bit(PG_t1, &(page)->flags)

#define PageLongTerm(page)	test_bit(PG_longterm, &(page)->flags)
#define SetPageLongTerm(page)	set_bit(PG_longterm, &(page)->flags)
#define TestSetPageLongTerm(page) test_and_set_bit(PG_longterm, &(page)->flags)
#define ClearPageLongTerm(page)	clear_bit(PG_longterm, &(page)->flags)
#define TestClearPageLongTerm(page) test_and_clear_bit(PG_longterm, &(page)->flags)

#define PageNew(page)		test_bit(PG_new, &(page)->flags)
#define SetPageNew(page)	set_bit(PG_new, &(page)->flags)
#define TestSetPageNew(page)	test_and_set_bit(PG_new, &(page)->flags)
#define ClearPageNew(page)	clear_bit(PG_new, &(page)->flags)
#define TestClearPageNew(page)	test_and_clear_bit(PG_new, &(page)->flags)

static DEFINE_PER_CPU(unsigned long, cart_nr_q);

/*
void  page_replace_init_cart(void)
{
	int i;

	for_each_cpu(i, cpu_possible_mask)
		per_cpu(cart_nr_q, i) = 0;
}
*/

#define CART_BASE 0
#define CART_LONG 1

enum cart_list {
	CART_T1 = CART_BASE,
	CART_T2 = CART_BASE+CART_LONG,
	CART_FILL1,
	CART_FILL2,
	CART_UNEVICTABLE,
	NR_CART_LISTS
};

struct lruvec {
	/* 
	 * The first field of the lruvec must be a pointer either to a zone or
	 * to a mem_cgroup_per_zone (mz) so we can retrive the zone/mz it
	 * belongs to.
	 */
	union {
		struct zone *zone;
		struct mem_cgroup_per_zone *mz;
	};
	struct list_head lists[NR_CART_LISTS];

	unsigned long nr_shortterm;
	unsigned long nr_p;

	unsigned long flags;
};

#define for_each_cart(cart) for (cart = 0; cart < NR_CART_LISTS; cart++)
#define for_each_evictable_cart(cart) for (cart = 0; cart <= CART_T2; cart++)

static int get_cart(enum cart_list cart)
{
	if (cart == CART_UNEVICTABLE)
		return CART_UNEVICTABLE;
	return cart;
}

struct list_head *get_cart_list(struct lruvec *lruvec, int lru)
{
	return &(lruvec->lists[lru]);
}

static unsigned long get_cart_size(struct lruvec *lruvec, enum cart_list lru)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, get_cart(lru));

	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + get_cart(lru));
}

static inline void page_replace_clear_state(struct page *page)
{
	if (PageT1(page))
		ClearPageT1(page);
	if (PageLongTerm(page))
		ClearPageLongTerm(page);
	if (PageNew(page))
		ClearPageNew(page);
}

static inline void page_replace_remove(struct lruvec *lruvec, struct page *page)
{
	int nr_pages = hpage_nr_pages(page);

	list_del(&page->lru);
	if (PageT1(page))
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE, -nr_pages);
	else
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1, -nr_pages);

	if (!PageLongTerm(page))
		lruvec->nr_shortterm -= nr_pages;

	page_replace_clear_state(page);
}

static inline unsigned long __page_replace_nr_pages(struct lruvec *lruvec)
{
	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) +
				zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + 1);
//	return lruvec->nr_T1 + lruvec->nr_T2;
}

static inline unsigned long __page_replace_nr_scan(struct lruvec *lruvec)
{
	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) +
				zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + 1);
//	return lruvec->nr_T1 + lruvec->nr_T2;
}

static void init_lruvec_cart(struct lruvec **lruvec, struct zone *zone)
{
	int i = 0;

	*lruvec = (struct lruvec *)alloc_bootmem(sizeof(struct lruvec));
	if (!(*lruvec)) {
		printk("init_lruvec_cart(): Couldn't allocate memory for lruvec.\n");
		return;
	}
	INIT_LIST_HEAD(&((*lruvec)->lists[CART_T1]));
	INIT_LIST_HEAD(&((*lruvec)->lists[CART_T2]));
	INIT_LIST_HEAD(&((*lruvec)->lists[CART_UNEVICTABLE]));

	(*lruvec)->nr_p = 0;

	(*lruvec)->nr_shortterm  = 0;
	(*lruvec)->flags = 0;

	for_each_cpu(i, cpu_possible_mask)
		per_cpu(cart_nr_q, i) = 0;
	
//	printk("CART: finish init lruvec\n");
}

static inline unsigned long cart_c(struct lruvec *lruvec)
{
//	printk(KERN_ERR "pages: T1: %ld, T2: %ld, Free: %ld, shortterm: %ld\n",
//				zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE),
//		zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1),
//		zone_page_state(lruvec_zone(lruvec), NR_FREE_PAGES), lruvec->nr_shortterm) ;

	return	zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) +
		zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1) +
		zone_page_state(lruvec_zone(lruvec), NR_FREE_PAGES) ;
}

#define scale(x, y, z) ({ unsigned long long tmp = (x); \
			  tmp *= (y); \
			  do_div(tmp, (z)); \
			  (unsigned long)tmp; })

#define B2T(x) scale((x), cart_c(lruvec), nonresident_total())
#define T2B(x) scale((x), nonresident_total(), cart_c(lruvec))

static inline unsigned long cart_longterm(struct lruvec *lruvec)
{
	return	zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) +
		zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1) -
		lruvec->nr_shortterm;
}

static inline unsigned long __cart_q(void)
{
	return __sum_cpu_var(unsigned long, cart_nr_q);
}

static void __cart_q_inc(struct lruvec *lruvec, unsigned long dq)
{
	/* if (|T2| + |B2| + |T1| - ns >= c) q = min(q + 1, 2c - |T1|) */
	/*     |B2| + nl               >= c                            */
	if (B2T(nonresident_count(NR_b2)) + cart_longterm(lruvec) >=
	    cart_c(lruvec)) {
		unsigned long nr_q = __cart_q();
	//	unsigned long target = 2*nonresident_total() - T2B(lruvec->nr_T1);
		unsigned long target = 2*nonresident_total() - 
							T2B(zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE));

		__get_cpu_var(cart_nr_q) += dq;
		nr_q += dq;

		if (nr_q > target) {
			unsigned long tmp = nr_q - target;
			__get_cpu_var(cart_nr_q) -= tmp;
		}
	}
}

static void __cart_q_dec(struct lruvec *lruvec, unsigned long dq)
{
	/* q = max(q - 1, c - |T1|) */
	unsigned long nr_q = __cart_q();
//	unsigned long target = nonresident_total() - T2B(lruvec->nr_T1);
	unsigned long target = nonresident_total() - 
							T2B(zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE));

	if (nr_q < dq) {
		__get_cpu_var(cart_nr_q) -= nr_q;
		nr_q = 0;
	} else {
		__get_cpu_var(cart_nr_q) -= dq;
		nr_q -= dq;
	}

	if (nr_q < target) {
		unsigned long tmp = target - nr_q;
		__get_cpu_var(cart_nr_q) += tmp;
	}
}

static inline unsigned long cart_q(void)
{
	unsigned long q;
	preempt_disable();
	q = __cart_q();
	preempt_enable();
	return q;
}

static inline void __cart_p_inc(struct lruvec *lruvec)
{
	/* p = min(p + max(1, ns/|B1|), c) */
	unsigned long ratio;
	ratio = (lruvec->nr_shortterm /
		 (B2T(nonresident_count(NR_b1)) + 1)) ?: 1UL;
	lruvec->nr_p += ratio;
	if (unlikely(lruvec->nr_p > cart_c(lruvec)))
		lruvec->nr_p = cart_c(lruvec);
}

static inline void __cart_p_dec(struct lruvec *lruvec)
{
	/* p = max(p - max(1, nl/|B2|), 0) */
	unsigned long ratio;
	ratio = (cart_longterm(lruvec) /
		 (B2T(nonresident_count(NR_b2)) + 1)) ?: 1UL;
	if (lruvec->nr_p >= ratio)
		lruvec->nr_p -= ratio;
	else
		lruvec->nr_p = 0UL;
}

static unsigned long list_count_flag(struct list_head *list, int PG_flag, int result)
{
	unsigned long nr = 0;
	struct page *page;
	list_for_each_entry(page, list, lru) {
		if (!!test_bit(PG_flag, &(page)->flags) == result)	//Quan,  determine state or flag
			++nr;
	}
	return nr;
}

static unsigned long list_count_state(struct list_head *list, int PG_flag, int result)
{
	unsigned long nr = 0;
	struct page *page;
	list_for_each_entry(page, list, lru) {
		if (!!test_bit(PG_flag, &(page)->flags) == result)	//Quan,  determine state or flag
			++nr;
	}
	return nr;
}

static void __validate_zone(struct lruvec *lruvec)
{
#if 0
	int nr_T1 = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE);
	int nr_T2 = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1);
	int bug = 0;
	unsigned long cnt0 = list_count_flag(&(lruvec->lists[CART_T1]), PG_lru, 0);
	unsigned long cnt1 = list_count_flag(&(lruvec->lists[CART_T1]), PG_lru, 1);

	if (cnt1 != nr_T1) {
		printk(KERN_ERR "__validate_zone: T1: %lu,%lu,%d\n", cnt0, cnt1, 
			nr_T1);
		bug = 1;
	}

	cnt0 = list_count_flag(&(lruvec->lists[CART_T2]), PG_lru, 0);
	cnt1 = list_count_flag(&(lruvec->lists[CART_T2]), PG_lru, 1);

	if (cnt1 != nr_T2 || bug) {
		printk(KERN_ERR "__validate_zone: T2: %lu,%lu,%d\n", cnt0, cnt1, nr_T2);
		bug = 1;
	}

	cnt0 = list_count_state(&(lruvec->lists[CART_T1]), PG_longterm, 0) +
			list_count_state(&(lruvec->lists[CART_T2]), PG_longterm, 0);
	cnt1 = list_count_state(&(lruvec->lists[CART_T1]), PG_longterm, 1) +
			list_count_state(&(lruvec->lists[CART_T2]), PG_longterm, 1);
	
	if (cnt0 != lruvec->nr_shortterm || bug) {
		printk(KERN_ERR "__validate_zone: shortterm: %lu,%lu,%ld\n", cnt0, cnt1, 
				lruvec->nr_shortterm);
		bug = 1;
	}

	cnt0 = list_count_state(&(lruvec->lists[CART_T2]), PG_longterm, 0);
	cnt1 = list_count_state(&(lruvec->lists[CART_T2]), PG_longterm, 1);
	if (cnt1 != nr_T2 || bug) {
		printk(KERN_ERR "__validate_zone: longterm: %lu,%lu,%d\n", cnt0, cnt1, 
				nr_T2);
		bug = 1;
	}

	if (bug) {
		BUG();
	}
#endif
}

/*
 * Insert page into @zones CART and update adaptive parameters.
 *
 * @zone: target zone.
 * @page: new page.
 */
 void __page_replace_add(struct zone *zone, struct page *page)
{
	int nr_pages;
	unsigned int rflags;

	/*
	 * Note: we could give hints to the insertion process using the LRU
	 * specific PG_flags like: PG_t1, PG_longterm and PG_referenced.
	 */
	struct lruvec *lruvec = zone->lruvecs[zone->policy];

	nr_pages = hpage_nr_pages(page);

	rflags = nonresident_get(page_mapping(page), page_index(page));

	if (rflags & NR_found) {
		SetPageLongTerm(page);
		rflags &= NR_listid;
		if (rflags == NR_b1) {
			__cart_p_inc(lruvec);
		} else if (rflags == NR_b2) {
			__cart_p_dec(lruvec);
			__cart_q_inc(lruvec, 1);
		}
		/* ++cart_longterm(zone); */
	} else {
		ClearPageLongTerm(page);
		++lruvec->nr_shortterm;
	}
	SetPageT1(page);

	list_add( &page->lru, &(lruvec->lists[CART_T1]) );

	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE, nr_pages);

//	printk(KERN_ERR "add a new page into T1\n");
	//	++ lruvec->nr_T1;
	BUG_ON(!PageLRU(page));

	__validate_zone(lruvec);
}

/*
static DEFINE_PER_CPU(struct pagevec, cart_add_pvecs) = { 0, };

void page_replace_add(struct page *page)
{
	struct pagevec *pvec = &get_cpu_var(cart_add_pvecs);

	page_cache_get(page);
	if (!pagevec_add(pvec, page))
		__pagevec_page_replace_add(pvec);
	put_cpu_var(cart_add_pvecs);
}

void __page_replace_add_drain(unsigned int cpu)
{
	struct pagevec *pvec = &per_cpu(cart_add_pvecs, cpu);

	if (pagevec_count(pvec))
		__pagevec_page_replace_add(pvec);
}
*/

#ifdef CONFIG_NUMA
//static void drain_per_cpu(void *dummy)
//{
//	page_replace_add_drain();
//}

/*
 * Returns 0 for success
 */
#if 0
int page_replace_add_drain_all(void)
{
//	return schedule_on_each_cpu(drain_per_cpu, NULL);
	return schedule_on_each_cpu(drain_per_cpu);
}
#endif
#else

/*
 * Returns 0 for success
 */
//int page_replace_add_drain_all(void)
//{
//	page_replace_add_drain();
//	return 0;
//}
#endif

void del_page_from_list(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, get_cart(lru), -nr_pages);
	list_del(&page->lru);
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + get_cart(lru),
				-nr_pages);
	
}

void add_page_to_list(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, lru, nr_pages);
	list_add(&page->lru, get_cart_list(lruvec, lru));
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru,
				nr_pages);
}


//#ifdef CONFIG_MIGRATION
/*
 * Isolate one page from the LRU lists and put it on the
 * indicated list with elevated refcount.
 *
 * Result:
 *  0 = page not on LRU list
 *  1 = page removed from LRU list and added to the specified list.
 */
static void isolate_cart(struct page *page, struct lruvec *lruvec)
//struct page *page)
{
//	printk(KERN_ERR "isolate cart\n");

	int nr_pages = hpage_nr_pages(page);

	get_page(page);
	ClearPageLRU(page);

	if (PageT1(page)) del_page_from_list(page, lruvec, CART_T1);
	else del_page_from_list(page, lruvec, CART_T2);
	
	if (!PageLongTerm(page))
		lruvec->nr_shortterm -= nr_pages;
}
//#endif

/*
 * Add page to a release pagevec, temp. drop zone lock to release pagevec if full.
 *
 * @zone: @pages zone.
 * @page: page to be released.
 * @pvec: pagevec to collect pages in.
 */
static inline void __page_release(struct lruvec *lruvec, struct page *page,
				       struct pagevec *pvec)
{
	int nr_pages = hpage_nr_pages(page);

//	if (TestSetPageLRU(page))
	if (PageLRU(page)) {
		BUG();
	}
	SetPageLRU(page);

	if (!PageLongTerm(page))
		lruvec->nr_shortterm += nr_pages;
//		++lruvec->nr_shortterm;
	if (PageT1(page))
		__mod_zone_page_state(lruvec_zone(lruvec), 
				NR_LRU_BASE, nr_pages);
//		++lruvec->nr_T1;
	else
		__mod_zone_page_state(lruvec_zone(lruvec), 
					NR_LRU_BASE+1, nr_pages);
//		++lruvec->nr_T2;
	
//	delete_from_history(page, lruvec->zone->history, HISTORY_EVICTABLE);

	if (!pagevec_add(pvec, page)) {
		spin_unlock_irq(&lruvec->zone->lru_lock);
//		if (buffer_heads_over_limit)
//			pagevec_strip(pvec);
		__pagevec_release(pvec);
		spin_lock_irq(&lruvec->zone->lru_lock);
	}
}

void putback_page_cart(struct page *page)
{
	struct zone *zone = page_zone(page);
	struct pagevec pvec;

	struct lruvec *lruvec = zone->lruvecs[PAGE_RECLAIM_CART];
	pagevec_init(&pvec, 1);
	
	printk(KERN_ERR "putback page cart\n");

/*
	list_for_each_entry_safe(page, page2, page_list, lru) {
		struct zone *pagezone = page_zone(page);

		if (pagezone != zone) {
			if (zone)
				spin_unlock_irq(&zone->lru_lock);
			zone = pagezone;
			spin_lock_irq(&zone->lru_lock);
		}
*/
		if (PageT1(page)) {
			list_move(&page->lru, &lruvec->lists[CART_T1]);
			dec_zone_page_state(page, NR_ISOLATED_ANON);
		} else {
			list_move(&page->lru, &lruvec->lists[CART_T2]);
			dec_zone_page_state(page, NR_ISOLATED_FILE);
		}
		__page_release(lruvec, page, &pvec);
//	}

//	if (zone)
//		spin_unlock_irq(&zone->lru_lock);
	pagevec_release(&pvec);
}

/*
 * zone->lru_lock is heavily contended.  Some of the functions that
 * shrink the lists perform better by taking out a batch of pages
 * and working on them outside the LRU lock.
 *
 * For pagecache intensive workloads, this function is the hottest
 * spot in the kernel (apart from copy_*_user functions).
 *
 * Appropriate locks must be held before calling this function.
 *
 * @nr_to_scan:	The number of pages to look through on the list.
 * @src:	The LRU list to pull pages off.
 * @dst:	The temp list to put pages on to.
 * @scanned:	The number of pages that were scanned.
 *
 * returns how many pages were moved onto *@dst.
 */
static unsigned long isolate_pages(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		isolate_mode_t mode, enum cart_list lru) 
{
//static int isolate_pages(struct lruvec *lruvec, int nr_to_scan,
//			 struct list_head *src,
//			 struct list_head *dst, int *scanned)
	struct list_head *src;
	int nr_taken;
	unsigned long scan;
	struct page *page;
	unsigned long nr_pages;
	
	scan = 0;
	nr_taken = 0;
	src = &lruvec->lists[lru];


	for(scan = 0; scan < nr_to_scan && !list_empty(src); scan++) {
		page = lru_to_page_cart(src);
		prefetchw_prev_lru_page_cart(page, src, flags);

//		if (!TestClearPageLRU(page))
		if (!PageLRU(page)) {
			printk(KERN_ERR "! Page LRU \n");
			__ClearPageLRU(page);
			BUG();
		}

		switch (__isolate_lru_page(page, mode)) {
			case 0:
				nr_pages = hpage_nr_pages(page);
				mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
				list_move(&page->lru, dst);
				nr_taken += nr_pages;
				if(!PageLongTerm(page))
					lruvec->nr_shortterm -= nr_pages;
				break;
			case -EBUSY:
				__put_page(page);	//Not sure.

				SetPageLRU(page);
				list_move(&page->lru, src);
				continue;
			default:
				BUG();
		}
	}

//	if(lru == CART_T2)
//		printk(KERN_ERR "isolate_pages in cart: nr taken %d, from cart list %d\n", 
//										nr_taken, lru);

//	zone->pages_scanned += scan;
	if ( lru == CART_T1 ) {
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+CART_T1, -nr_taken);
		__mod_zone_page_state(lruvec_zone(lruvec), NR_ISOLATED_ANON, nr_taken);
	} else {
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+CART_T2, -nr_taken);
		__mod_zone_page_state(lruvec_zone(lruvec), NR_ISOLATED_ANON + 1, nr_taken);
	}

	*nr_scanned = scan;
	return nr_taken;
}

static int cart_reclaim_T1(struct lruvec *lruvec)
{
	int ret = 0;
	int t1 = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) > lruvec->nr_p;
//	int t1 = lruvec->nr_T1 > lruvec->nr_p;
	int sat = TestClearZoneSaturated(lruvec);
	int rec = ZoneReclaimedT1(lruvec);

	if ((t1 && !(rec && sat)) ||
	    (!t1 && (!rec && sat)))
			ret = 1;
	
	return ret;
/*
	if (t1) {
		if (sat && rec)
			return 0;
		return 1;
	}

	if (sat && !rec)
		return 1;
	return 0;
*/
}


//void page_replace_candidates(struct lruvec *lruvec, int nr_to_scan,
//		struct list_head *page_list)
static unsigned long
page_replace_candidates(unsigned long nr_to_scan, 
				struct lruvec *lruvec, struct scan_control *sc,
		struct list_head *page_list)
{
	unsigned long nr_scan;
	unsigned long nr_taken;
	enum cart_list cart;

//	struct list_head *list;
	struct zone *zone = lruvec_zone(lruvec);

	isolate_mode_t isolate_mode = 0;
	
	lru_add_drain();
//	page_replace_add_drain();
	
	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	spin_lock_irq(&zone->lru_lock);


//	if(cart == CART_T1) SetZoneReclaimedT1(lruvec);
//	else if(cart == CART_T2) ClearZoneReclaimedT1(lruvec);
	
	if (cart_reclaim_T1(lruvec)) {
		cart = CART_T1;
//		list = &(lruvec->lists[CART_T1]);
		SetZoneReclaimedT1(lruvec);
	} else {
//		printk(KERN_ERR "reclaim T2 pages----------------------\n");
		cart = CART_T2;
//		list = &(lruvec->lists[CART_T2]);
		ClearZoneReclaimedT1(lruvec);
	}

	nr_taken = isolate_pages(nr_to_scan, lruvec, page_list, &nr_scan, sc,
			         isolate_mode, cart);
	
	if (!nr_taken) {
//		if (list == &(lruvec->lists[CART_T1])) {
		if (cart == CART_T1) {
			cart = CART_T2;
//			list = &(lruvec->lists[CART_T2]);
			ClearZoneReclaimedT1(lruvec);
		} else {
			cart = CART_T1;
//			list = &(lruvec->lists[CART_T1]);
			SetZoneReclaimedT1(lruvec);
		}

	nr_taken = isolate_pages(nr_to_scan, lruvec, page_list, &nr_scan, sc,
			         isolate_mode, cart);
//		nr_taken = isolate_pages(lruvec, nr_to_scan, list,
//				         page_list, &nr_scan);
	}

	if (global_reclaim(sc)) {
		zone->pages_scanned += nr_scan;
		if (current_is_kswapd())
			__count_zone_vm_events(PGSCAN_KSWAPD, zone, nr_scan);
		else
			__count_zone_vm_events(PGSCAN_DIRECT, zone, nr_scan);
	}
	
	spin_unlock_irq(&zone->lru_lock);
//	local_irq_enable();

	return nr_taken;
}

static void page_replace_reinsert_zone(struct lruvec *lruvec, struct list_head *page_list,
										int nr_freed)
{
	struct pagevec pvec;
	unsigned long dqi = 0;
	unsigned long dqd = 0;
	unsigned long dsl = 0;
	unsigned long target;
	int nr_pages;

	int nr_T1 = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE);
//	int nr_T2 = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1);


	pagevec_init(&pvec, 1);
	spin_lock_irq(&lruvec->zone->lru_lock);

	target = min(lruvec->nr_p + 1UL, B2T(nonresident_count(NR_b1)));
	
//	printk(KERN_ERR "page replace reinsert zone in cart: target %ld\n", target);

	while (!list_empty(page_list)) {
		struct page * page = lru_to_page_cart(page_list);
		prefetchw_prev_lru_page_cart(page, page_list, flags);
	
		nr_pages = hpage_nr_pages(page);

		if (PageT1(page)) { /* T1 */
			if (TestClearPageReferenced(page)) {
				if (!PageLongTerm(page) &&
				    (nr_T1 - dqd + dqi) >= target) {
					SetPageLongTerm(page);
					dsl += nr_pages;
				}
				list_move( &page->lru, &(lruvec->lists[CART_T1]) );
			} else if (PageLongTerm(page)) {
				ClearPageT1(page);
				dqd += nr_pages;
				list_move( &page->lru, &(lruvec->lists[CART_T2]) );
			} else {
				/* should have been reclaimed or was PG_new */
				list_move( &page->lru, &(lruvec->lists[CART_T1]) );
			}
		} else { /* T2 */
			if (TestClearPageReferenced(page)) {
				SetPageT1(page);
				dqi += nr_pages;
				list_move( &page->lru, &(lruvec->lists[CART_T1]) );
			} else {
				/* should have been reclaimed */
				ClearPageT1(page);
				list_move( &page->lru, &(lruvec->lists[CART_T2]) );
			}
		}
		__page_release(lruvec, page, &pvec);
	}

	if (!nr_freed) SetZoneSaturated(lruvec);

	if (dqi > dqd)
		__cart_q_inc(lruvec, dqi - dqd);
	else
		__cart_q_dec(lruvec, dqd - dqi);

	spin_unlock_irq(&lruvec->zone->lru_lock);
	pagevec_release(&pvec);
}

#if 0
void __page_replace_rotate_reclaimable(struct lruvec *lruvec, struct page *page)
{
	int nr_pages = hpage_nr_pages(page);

	if (PageLRU(page)) {
		if (CART_PageLongTerm(page)) {
			if (CART_TestClearPageT1(page)) {
				__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE, -nr_pages);
				__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1, nr_pages);
				__cart_q_dec(lruvec, nr_pages);
			}
			list_move_tail( &page->lru, &(lruvec->lists[CART_T2]) );
		} else {
			if (!CART_PageT1(page))
				BUG();
			list_move_tail( &page->lru, &(lruvec->lists[CART_T1]) );
		}
	}
}
#endif 

void nonres_remember_cart(struct zone *zone, struct page *page)
{
	struct lruvec *lruvec = zone->lruvecs[PAGE_RECLAIM_CART];

	int target_list = PageT1(page) ? NR_b1 : NR_b2;
	int evict_list = (nonresident_count(NR_b1) > cart_q())
		? NR_b1 : NR_b2;

/*
	if(target_list)
	printk(KERN_ERR "page replace remember CART: target %d, evict %d, NR b1 %ld, NR b2 %ld, T1 count %lu, T2 count %lu, p is %ld, q is %ld\n", 
			target_list, evict_list, nonresident_count(NR_b1), 
			nonresident_count(NR_b2), 
			zone_page_state(zone, NR_LRU_BASE+CART_T1),
			zone_page_state(zone, NR_LRU_BASE+CART_T2),
			lruvec->nr_p,
			__cart_q()
			);
*/	
	nonresident_put(page_mapping(page), page_index(page),
			target_list, evict_list);
}

static void nonres_forget_cart(struct address_space *mapping, unsigned long index)
{
	nonresident_get(mapping, index);
}

#define K(x) ((x) << (PAGE_SHIFT-10))

/*
void page_replace_show(struct zone *zone)
{
	printk("%s"
	       " free:%lukB"
	       " min:%lukB"
	       " low:%lukB"
	       " high:%lukB"
	       " T1:%lukB"
	       " T2:%lukB"
	       " shortterm:%lukB"
	       " present:%lukB"
	       " pages_scanned:%lu"
	       " all_unreclaimable? %s"
	       "\n",
	       zone->name,
	       K(zone->free_pages),
	       K(zone->pages_min),
	       K(zone->pages_low),
	       K(zone->pages_high),
	       K(zone->policy.nr_T1),
	       K(zone->policy.nr_T2),
	       K(zone->policy.nr_shortterm),
	       K(zone->present_pages),
	       zone->pages_scanned,
	       (zone->all_unreclaimable ? "yes" : "no")
	      );
}

void page_replace_zoneinfo(struct zone *zone, struct seq_file *m)
{
	seq_printf(m,
		   "\n  pages free       %lu"
		   "\n        min        %lu"
		   "\n        low        %lu"
		   "\n        high       %lu"
		   "\n        T1         %lu"
		   "\n        T2         %lu"
		   "\n        shortterm  %lu"
		   "\n        p          %lu"
		   "\n        flags      %lu"
		   "\n        scanned    %lu"
		   "\n        spanned    %lu"
		   "\n        present    %lu",
		   zone->free_pages,
		   zone->pages_min,
		   zone->pages_low,
		   zone->pages_high,
		   zone->policy.nr_T1,
		   zone->policy.nr_T2,
		   zone->policy.nr_shortterm,
		   zone->policy.nr_p,
		   zone->policy.flags,
		   zone->pages_scanned,
		   zone->spanned_pages,
		   zone->present_pages);
}

void __page_replace_counts(unsigned long *active, unsigned long *inactive,
			   unsigned long *free, struct pglist_data *pgdat)
{
	struct zone *zones = pgdat->node_zones;
	int i;

	*active = 0;
	*inactive = 0;
	*free = 0;
	for (i = 0; i < MAX_NR_ZONES; i++) {
		*active += zones[i].policy.nr_T1 + zones[i].policy.nr_T2 -
			zones[i].policy.nr_shortterm;
		*inactive += zones[i].policy.nr_shortterm;
		*free += zones[i].free_pages;
	}
}
*
*/

struct list_head* get_test_list_cart(struct lruvec *lruvec, int lru)
{
	return &lruvec->lists[lru];
}

void print_lruvec_cart(struct zone *zone)
{
	printk("CART: Helper Function.\n");
}

void get_scan_count_cart(struct lruvec *lruvec, struct scan_control *sc,
				unsigned long *nr, bool force_scan) 
{
	unsigned long nr_scan;

	nr_scan = get_cart_size(lruvec, CART_T1) +
				get_cart_size(lruvec, CART_T2);

	nr_scan >>= sc->priority;
	nr_scan ++;

	nr[CART_T1] = nr_scan;
	nr[CART_T2] = 0;

/*
	unsigned long t1;
	unsigned long t2;

	t1 = get_cart_size(lruvec, CART_T1);	
	t2 = get_cart_size(lruvec, CART_T2);	

	if(sc->priority) {
		t1 >>= sc->priority;
		t2 >>= sc->priority;
		if(!t1 && force_scan)
			t1 = SWAP_CLUSTER_MAX;
		if(!t2 && force_scan)
			t2 = SWAP_CLUSTER_MAX;
	}

	nr[CART_T1] = t1;
	nr[CART_T2] = t2;
*/

}

int too_many_isolated(struct zone *zone, int file,
		struct scan_control *sc)
{
	unsigned long inactive, isolated;

	if (current_is_kswapd())
		return 0;

	if (!global_reclaim(sc))
		return 0;

	if (file) {
//		inactive = zone_page_state(zone, NR_INACTIVE_FILE);
		inactive = zone_page_state(zone, NR_LRU_BASE+1);
		isolated = zone_page_state(zone, NR_ISOLATED_FILE);
	} else {
		inactive = zone_page_state(zone, NR_LRU_BASE);
//		inactive = zone_page_state(zone, NR_INACTIVE_ANON);
		isolated = zone_page_state(zone, NR_ISOLATED_ANON);
	}

	/*
	 * GFP_NOIO/GFP_NOFS callers are allowed to isolate more pages, so they
	 * won't get blocked by normal direct-reclaimers, forming a circular
	 * deadlock.
	 */
	if ((sc->gfp_mask & GFP_IOFS) == GFP_IOFS)
		inactive >>= 3;

	if(isolated > inactive)
		printk(KERN_ERR "isolated: %ld, inactive %ld, is T2: %d\n", isolated, inactive, file);

	return isolated > inactive;
}

bool too_many_isolated_comapction_cart(struct zone *zone)
{
	unsigned long all, isolated;

	all = zone_page_state(zone, NR_LRU_BASE)+zone_page_state(zone, NR_LRU_BASE+1);
	isolated = zone_page_state(zone, NR_ISOLATED_ANON)+zone_page_state(zone, NR_ISOLATED_ANON+1);

	return isolated > all;
}

inline int is_T2_cart(enum cart_list cart)
{
	return (cart == CART_T2);
}

noinline_for_stack unsigned long 
shrink_list_cart (unsigned long nr_to_scan, struct lruvec *lruvec,
		struct scan_control *sc, enum cart_list cart) 
{
	LIST_HEAD(page_list);
	unsigned long nr_reclaimed = 0;
	unsigned long nr_taken;
	unsigned long nr_dirty = 0;
	unsigned long nr_writeback = 0;

//	int is_T2 = is_T2_cart(cart);
	struct zone *zone = lruvec_zone(lruvec);

/*
	while (unlikely(too_many_isolated(zone, is_T2, sc))) {
		congestion_wait(BLK_RW_ASYNC, HZ/10);

		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}
*/
	nr_taken = page_replace_candidates(nr_to_scan, lruvec, sc, &page_list);

//	nr_reclaimed = shrink_list(&page_list, sc);

	nr_reclaimed = shrink_page_list(&page_list, zone, sc, TTU_UNMAP, 
			&nr_dirty, &nr_writeback, false);

//	local_irq_disable();
	spin_lock_irq(&zone->lru_lock);
	
	if (global_reclaim(sc)) {
		if (current_is_kswapd())
			__count_zone_vm_events(PGSTEAL_KSWAPD, zone,
					       nr_reclaimed);
		else
			__count_zone_vm_events(PGSTEAL_DIRECT, zone,
					       nr_reclaimed);
	}
	
//	__mod_zone_page_state(zone, NR_ISOLATED_ANON + is_T2, -nr_taken);
	
	spin_unlock_irq(&zone->lru_lock);

	page_replace_reinsert_zone(lruvec, &page_list, nr_reclaimed);
	
	if (nr_writeback && nr_writeback >=
			(nr_taken >> (DEF_PRIORITY - sc->priority)))
		wait_iff_congested(zone, BLK_RW_ASYNC, HZ/10);
//	
	throttle_vm_writeout(sc->gfp_mask);

	return nr_reclaimed;
}

static int shrink_lruvec_cart(struct lruvec *lruvec, struct scan_control *sc,
					unsigned long *nr)
{
	unsigned long nr_to_scan;
//	enum cart_list cart;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;
	
//	printk(KERN_ERR "shrink lruvec cart: %ld, %ld\n", nr[CART_T1], nr[CART_T2]);

	while (nr[CART_T1] >= SWAP_CLUSTER_MAX) {
		nr_to_scan = min_t(unsigned long, nr[CART_T1], SWAP_CLUSTER_MAX);

		nr[CART_T1] -= nr_to_scan;
	
		nr_reclaimed +=shrink_list_cart(nr_to_scan, lruvec, sc, CART_T1);
		
		if(nr_reclaimed >= nr_to_reclaim && sc->priority < DEF_PRIORITY)
			break;
	}
/*	
	while (nr[CART_T1] || nr[CART_T2]) {
		for_each_evictable_cart(cart) {
			if(nr[cart]) {
				nr_to_scan = min_t(unsigned long, nr[cart], SWAP_CLUSTER_MAX);
				nr[cart] -= nr_to_scan;

				nr_reclaimed +=shrink_list_cart(nr_to_scan, lruvec, sc, cart);
			}
		}

		if(nr_reclaimed >= nr_to_reclaim && sc->priority < DEF_PRIORITY)
			break;
	}
*/
	return nr_reclaimed;
}

void balance_lruvec_cart(struct lruvec *lruvec, struct scan_control *sc)
{

}

bool should_continue_reclaim_cart(struct lruvec *lruvec, 
								unsigned long nr_reclaimed,
								unsigned long nr_scanned,
								struct scan_control *sc)
{
//	unsigned long reclaimable_pages;
	unsigned long pages_for_compaction = (2UL << sc->order);

	printk("should_continue_reclaim_cart():pages_for_compaction:%lu\n", 
						pages_for_compaction);

//	reclaimable_pages = get_fifo_size(lruvec, FIFO_EVICTABLE);
//	if (sc->nr_reclaimed < pages_for_compaction && 
//	    reclaimable_pages > pages_for_compaction)
//		return true;

	return false;

}

unsigned long global_reclaimable_pages_cart(void)
{
	return global_page_state(NR_LRU_BASE);
}

unsigned long zone_reclaimable_pages_cart(struct zone *zone)
{
	return zone_page_state(zone, NR_LRU_BASE);
}

void page_accessed_cart(struct page *page)
{
	SetPageReferenced(page);
	/* A page has been accessed. We don't do anything in FIFO. */
}

void activate_page_cart(struct page *page, struct lruvec *lruvec)
{
	/* Don't do anything if the page is accessed a second time, which means
	 * it's activated. */

	if(!TestClearPageNew(page)) {
		SetPageReferenced(page);
	}
}

void deactivate_page_cart(struct page *page, struct lruvec *lruvec,
				 bool page_writeback_or_dirty)
{
	/* Don't do anything if the page has been inactive for a while.
	 * It's FIFO. */
}

void update_reclaim_statistics_cart(struct lruvec *lruvec, int type,
					   int rotated)
{
	/* We don't account for page statistics for now. */
}


void add_page_to_list_cart(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	add_page_to_list(page, lruvec, lru);
}

void del_page_from_list_cart(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	del_page_from_list(page, lruvec, lru);
}

void add_page_cart(struct page *page, struct lruvec *lruvec, int lru)
{
	//added by quan
	unsigned int rflags;
	
	if(!PageLRU(page)) {
		printk(KERN_ERR "the page is not in LRU\n");
		BUG_ON(!PageLRU(page));
//		SetPageLRU(page);
	}

	rflags = nonresident_get(page_mapping(page), page_index(page));

	if(rflags & NR_found) {
		SetPageLongTerm(page);
		rflags &= NR_listid;

		if(rflags == NR_b1) {
			__cart_p_inc(lruvec);
		} else if (rflags == NR_b2) {
			__cart_p_dec(lruvec);
			__cart_q_inc(lruvec, 1);
		}
	} else {
		ClearPageLongTerm(page);
		++lruvec->nr_shortterm;
	}

	SetPageT1(page);

//	add_page_to_list(page, lruvec, lru);
	add_page_to_list(page, lruvec, CART_T1);

//	BUG_ON(!PageLRU(page));
	__validate_zone(lruvec);
}

void release_page_cart(struct page *page, struct lruvec *lruvec, int lru,
			     bool batch_release)
{
	int nr_pages = hpage_nr_pages(page);

	int is_T1 = PageT1(page);
/*
	printk(KERN_ERR "release page: is mru %d\n", is_mru);
*/
	if(is_T1)
		del_page_from_list_cart(page, lruvec, CART_T1);
	else 
		del_page_from_list_cart(page, lruvec, CART_T2);
//	printk(KERN_ERR "Release_page arc: del page from list %d %d\n", is_mru, is_mfu);

	if (!PageLongTerm(page))
		lruvec->nr_shortterm -= nr_pages;

	page_replace_clear_state(page);

	if(is_T1)	ClearPageT1(page);
	
//	del_page_from_list(page, lruvec, lru);
}

void rotate_inactive_page_cart(struct page *page, struct lruvec *lruvec)
{
	int nr_pages = hpage_nr_pages(page);

	if (PageLRU(page)) {
		if (PageLongTerm(page)) {
			if (TestClearPageT1(page)) {
				__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE, -nr_pages);
				__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1, nr_pages);
				__cart_q_dec(lruvec, nr_pages);
			}
			list_move_tail( &page->lru, &(lruvec->lists[CART_T2]) );
		} else {
			if (!PageT1(page))
				BUG();
			list_move_tail( &page->lru, &(lruvec->lists[CART_T1]) );
		}
	}

	else {
		printk(KERN_ERR "Error in rotate inactive page cart\n");
	//To add
		if(PageT1(page))
			list_move_tail(&page->lru, &(lruvec->lists[CART_T1]));
		else 
			list_move_tail(&page->lru, &(lruvec->lists[CART_T2]));
	}
//	list_move_tail(&page->lru, &lruvec->lists[FIFO_EVICTABLE]);
}

void reset_zone_vmstat_cart(struct lruvec *lruvec, struct zone *zone,
				   bool evictable)
{
	enum cart_list cart;
	int index, nr_pages, i=0;
	struct page *page;
//	int lru = evictable ? ARC_BASE : ARC_UNEVICTABLE;

	if(evictable) {
		for_each_evictable_cart(cart) {
			index = NR_LRU_BASE + cart;
			
			nr_pages = zone_page_state(zone, index);
			__mod_zone_page_state(zone, index, -nr_pages);
			mem_cgroup_update_lru_size(lruvec, cart, -nr_pages);
			printk(KERN_ERR "EVIC: %d, num of pages %d\n", cart, nr_pages);	
			
			list_for_each_entry(page, &(lruvec->lists[cart]), lru) {
				if(!PageUnevictable(page) && PageLRU(page)) {
					add_to_history(page,  zone->history, HISTORY_EVICTABLE);
				}
			}
		}
		nr_pages = zone_page_state(zone, NR_ISOLATED_ANON);
		__mod_zone_page_state(zone, NR_ISOLATED_ANON, -nr_pages);
		nr_pages = zone_page_state(zone, NR_ISOLATED_FILE);
		__mod_zone_page_state(zone, NR_ISOLATED_FILE, -nr_pages);
	} else {
		index = NR_LRU_BASE + CART_UNEVICTABLE;
		
		nr_pages = zone_page_state(zone, index);
		__mod_zone_page_state(zone, index, -nr_pages);
		mem_cgroup_update_lru_size(lruvec, CART_UNEVICTABLE, -nr_pages);
			
		list_for_each_entry(page, &(lruvec->lists[CART_UNEVICTABLE]), lru) {
			if(PageUnevictable(page)) {
				add_to_history(page,  zone->history, HISTORY_UNEVICTABLE);
			}
		}
			
		printk(KERN_ERR "UNEVIC: num of pages %d\n", nr_pages);	
	}

	i = 0;

	lruvec->nr_p = 0;

	lruvec->nr_shortterm  = 0;
	lruvec->flags = 0;

	for_each_cpu(i, cpu_possible_mask)
		per_cpu(cart_nr_q, i) = 0;
}

void add_page_unevictable_cart(struct page *page, struct lruvec *lruvec)
{
	add_page_to_list(page, lruvec, CART_UNEVICTABLE);
}

static int activate_cart(struct page *page)
{
	if (!TestClearPageNew(page)) {
		SetPageReferenced(page);
		SetPageActive(page);
		return 1;
	}
	
	return 0;

//	VM_BUG_ON(PageActive(page));
//	SetPageActive(page);
//	return 1;
}

static int page_check_references_cart(struct page *page, struct scan_control *sc)
{
	int referenced_ptes, referenced_page;
	unsigned long vm_flags;

	referenced_ptes = page_referenced(page, 1, sc->target_mem_cgroup,
					  &vm_flags);
	referenced_page = TestClearPageReferenced(page);

	if (referenced_ptes || referenced_page) {
		return PAGEREF_ACTIVATE;
	}

	if(PageNew(page))
		ClearPageNew(page);
	
	if((PageT1(page) && PageLongTerm(page)) ||
			(!PageT1(page) && !PageLongTerm(page)))
		return PAGEREF_KEEP;

	return PAGEREF_RECLAIM;
/*
	if (referenced_ptes) {
		if (PageSwapBacked(page))
			return PAGEREF_ACTIVATE;
		
		SetPageReferenced(page);

		if (referenced_page || referenced_ptes > 1)
			return PAGEREF_ACTIVATE;

		if (vm_flags & VM_EXEC)
			return PAGEREF_ACTIVATE;

		return PAGEREF_KEEP;
	}

	if (referenced_page && !PageSwapBacked(page))
		return PAGEREF_RECLAIM_CLEAN;

	return PAGEREF_RECLAIM;
*/
}

static inline void hint_use_once_cart(struct page *page) {
	if(PageLRU(page))
		BUG();
	SetPageNew(page);
}
const struct page_reclaim_policy cart_page_reclaim_policy =
{
	/* Initialize the structures. */
	.init_lruvec = init_lruvec_cart,

	/* Decide which pages to reclaim and actually do the reclaiming. */
	.get_scan_count = get_scan_count_cart,
	.shrink_lruvec = shrink_lruvec_cart,
	.balance_lruvec = balance_lruvec_cart,

	/* Helpers used when deciding which pages to reclaim and compact.*/
	.should_continue_reclaim = should_continue_reclaim_cart,
	.zone_reclaimable_pages = zone_reclaimable_pages_cart,
	.global_reclaimable_pages = global_reclaimable_pages_cart,
	.too_many_isolated_compaction = too_many_isolated_comapction_cart,

	/* Capture activity and statistics */
	.page_accessed = page_accessed_cart,
	.activate_page = activate_page_cart,
	.deactivate_page = deactivate_page_cart,
	.update_reclaim_statistics = update_reclaim_statistics_cart,

	/* Add/remove pages from the lists. */
	.add_page_to_list = add_page_to_list_cart,
	.del_page_from_list = del_page_from_list_cart,
	.add_page = add_page_cart,
	.release_page = release_page_cart,
	.add_page_unevictable = add_page_unevictable_cart,

	/* Helpers used for specific scenarios. */
	.rotate_inactive_page = rotate_inactive_page_cart,
	.get_lruvec_list = get_cart_list,
	.reset_zone_vmstat = reset_zone_vmstat_cart,

	/* For testing purposes */
	.get_list = get_test_list_cart,
	.print_lruvec = print_lruvec_cart,

	/*For non-resident lists*/
	.nonres_remember = nonres_remember_cart,
	.nonres_forget = nonres_forget_cart,
//
	/*isolate one page from the LRU lists*/
	.isolate = isolate_cart,
	.putback_page = putback_page_cart,

	/* New */
	.activate = activate_cart,
	.page_check_references = page_check_references_cart,
//	.hint_use_once = hint_use_once_cart,
};
EXPORT_SYMBOL(cart_page_reclaim_policy);
