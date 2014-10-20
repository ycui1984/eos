#ifdef CONFIG_RECLAIM_POLICY
/*
 * mm/arc.c
 *
 * In this implementation, we do not distinguish anonymous pages and file pages
 *
 * Written by Quan Chen <chen-quan@sjtu.edu.cn>, 
 * modified based on Peter Zijlstra's code <a.p.zijlstra@chello.nl>
 *
 * This file contains a Page Replacement Algorithm based on arc
 * Please refer to the arc paper here -
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
//#include <linux/mm_arc.h>
//#include <linux/mm_page_replace.h>

#define lru_to_page_arc(_head) (list_entry((_head)->prev, struct page, lru))

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page_arc(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page_arc(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

#define ARC_RECLAIMED_MRU 0
#define ARC_SATURATED 1

#define ZoneReclaimedMRU(z)	test_bit(ARC_RECLAIMED_MRU, &((z)->flags))
#define SetZoneReclaimedMRU(z)	__set_bit(ARC_RECLAIMED_MRU, &((z)->flags))
#define ClearZoneReclaimedMRU(z)	__clear_bit(ARC_RECLAIMED_MRU, &((z)->flags))

#define ZoneSaturated(z)	test_bit(ARC_SATURATED, &((z)->flags))
#define SetZoneSaturated(z)	__set_bit(ARC_SATURATED, &((z)->flags))
#define TestClearZoneSaturated(z)  __test_and_clear_bit(ARC_SATURATED, &((z)->flags))

#define PG_MRU		 PG_reclaim1
#define PG_SATURATED PG_reclaim2 
#define PG_NEW		 PG_reclaim3

#define PageMRU(page)		test_bit(PG_MRU, &(page)->flags)
#define SetPageMRU(page)		set_bit(PG_MRU, &(page)->flags)
#define ClearPageMRU(page)	clear_bit(PG_MRU, &(page)->flags)
#define TestClearPageMRU(page)	test_and_clear_bit(PG_MRU, &(page)->flags)
#define TestSetPageMRU(page)	test_and_set_bit(PG_MRU, &(page)->flags)

#define PageNew(page)		test_bit(PG_NEW, &(page)->flags)
#define SetPageNew(page)	set_bit(PG_NEW, &(page)->flags)
#define TestSetPageNew(page)	test_and_set_bit(PG_NEW, &(page)->flags)
#define ClearPageNew(page)	clear_bit(PG_NEW, &(page)->flags)
#define TestClearPageNew(page)	test_and_clear_bit(PG_NEW, &(page)->flags)

static DEFINE_PER_CPU(unsigned long, arc_nr_q);

#define ARC_BASE 0
#define ARC_FREQ 1

enum arc_list {
	ARC_MRU = ARC_BASE,
	ARC_MFU = ARC_BASE+ARC_FREQ,
	ARC_FILL1,
	ARC_FILL2,
	ARC_UNEVICTABLE,
	NR_ARC_LISTS
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
	struct list_head lists[NR_ARC_LISTS];

	unsigned long c;
	long p;

	long nr_MRU;
	long nr_MFU;

	unsigned long flags;
};

#define for_each_arc(arc) for (arc = 0; arc < NR_ARC_LISTS; arc++)
#define for_each_evictable_arc(arc) for (arc = 0; arc <= ARC_MFU; arc++)

static int get_arc(enum arc_list arc)
{
	return arc;
}

struct list_head *get_arc_list(struct lruvec *lruvec, int arc)
{
	return &(lruvec->lists[arc]);
}

static unsigned long get_arc_size(struct lruvec *lruvec, enum arc_list arc)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, get_arc(arc));

//	if (arc)	return lruvec->nr_MFU;
//	else return lruvec->nr_MRU;
	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + get_arc(arc));
}

static inline void page_replace_clear_state(struct page *page)
{
	if (PageMRU(page))
		ClearPageMRU(page);
	if (PageNew(page))
		ClearPageNew(page);
}

static inline void page_replace_remove(struct lruvec *lruvec, struct page *page)
{
	int nr_pages = hpage_nr_pages(page);

	printk(KERN_ERR "page replace remove\n");
	list_del(&page->lru);
	if (PageMRU(page)) {
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE, -nr_pages);
//		lruvec->nr_MRU -= nr_pages;
	} else {
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1, -nr_pages);
//		lruvec->nr_MFU -= nr_pages;
	}

	page_replace_clear_state(page);
}

static inline unsigned long __page_replace_nr_pages(struct lruvec *lruvec)
{
	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) +
				zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + 1);
//	return lruvec->nr_MRU + lruvec->nr_MFU;
}

static inline unsigned long __page_replace_nr_scan(struct lruvec *lruvec)
{
	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) +
				zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + 1);
//	return lruvec->nr_MRU + lruvec->nr_MFU;
}

static void init_lruvec_arc(struct lruvec **lruvec, struct zone *zone)
{
	int i = 0;
	*lruvec = (struct lruvec *)alloc_bootmem(sizeof(struct lruvec));
	if (!(*lruvec)) {
		printk(KERN_ERR "init_lruvec_arc(): Couldn't allocate memory for lruvec.\n");
		return;
	}
	memset(*lruvec, 0, sizeof(struct lruvec));

	INIT_LIST_HEAD(&((*lruvec)->lists[ARC_MRU]));
	INIT_LIST_HEAD(&((*lruvec)->lists[ARC_MFU]));
	INIT_LIST_HEAD(&((*lruvec)->lists[ARC_UNEVICTABLE]));

	(*lruvec)->c = nr_all_pages;

	(*lruvec)->p = 0;

	(*lruvec)->nr_MRU = (*lruvec)->nr_MFU = 0;

	(*lruvec)->flags = 0;
	
	for_each_cpu(i, cpu_possible_mask)
		per_cpu(arc_nr_q, i) = 0;
}

static inline unsigned long arc_c(struct lruvec *lruvec)
{
	return	zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) +
		zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1) +
		zone_page_state(lruvec_zone(lruvec), NR_FREE_PAGES) ;
//	return zone->policy.nr_T1 + zone->policy.nr_T2 + zone->free_pages;
}

#define scale(x, y, z) ({ unsigned long long tmp = (x); \
			  tmp *= (y); \
			  do_div(tmp, (z)); \
			  (unsigned long)tmp; })

#define B2T(x) scale((x), arc_c(lruvec), nonresident_total())
#define T2B(x) scale((x), nonresident_total(), arc_c(lruvec))

static inline unsigned long __arc_q(void)
{
	return __sum_cpu_var(unsigned long, arc_nr_q);
}

static void __arc_q_inc(struct lruvec *lruvec, unsigned long dq)
{
	if (B2T(nonresident_count(NR_b2)) + zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1) 
			>= arc_c(lruvec)) {
		unsigned long nr_q = __arc_q();
//		unsigned long target = nonresident_total() - T2B(lruvec->nr_T1);
		unsigned long target = nonresident_total() - T2B( zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) );
//		unsigned long target = 2*nonresident_total() - 
//							T2B(zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE));
		printk(KERN_ERR "arc_q inc: B2T B2: %ld, MFU: %ld, ARC_C %ld, target %ld, q is %ld, dq is %ld\n",
							B2T(nonresident_count(NR_b2)),
							zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1),
							arc_c(lruvec), target, nr_q, dq);

		__get_cpu_var(arc_nr_q) += dq;
		nr_q += dq;

		if (nr_q > target) {
			unsigned long tmp = nr_q - target;
			__get_cpu_var(arc_nr_q) -= tmp;
		}
	}
//	__get_cpu_var(arc_nr_q) ++;
}

static void __arc_q_dec(struct lruvec *lruvec, unsigned long dq)
{
///*
	unsigned long nr_q = __arc_q();
	unsigned long target = nonresident_total() - 
							T2B(zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE));
//	unsigned long target = T2B(zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE));
/*
	printk(KERN_ERR "arc_q dec: q %ld, T2B B1 %ld, MRU: %ld, ARC_C %ld, target %ld, dq is %ld\n",
						nr_q,
						T2B(zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE)),
						zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE),
						arc_c(lruvec), target, dq);
*/
	if (nr_q < dq) {
		__get_cpu_var(arc_nr_q) -= nr_q;
		nr_q = 0;
	} else {
		__get_cpu_var(arc_nr_q) -= dq;
		nr_q -= dq;
	}

	if (nr_q < target) {
		unsigned long tmp = target - nr_q;
		__get_cpu_var(arc_nr_q) += tmp;
	}
//*/		
//	__get_cpu_var(arc_nr_q) --;
}

static inline unsigned long arc_q(void)
{
	unsigned long q;
	preempt_disable();
	q = __arc_q();
	preempt_enable();
	return q;
}

static inline void __arc_p_inc(struct lruvec *lruvec, int nr_pages)
{
	/* p = min(p + max(1, ns/|B1|), c) */
	unsigned long ratio;
	long nr_rec, nr_freq;
	
	nr_rec = B2T(nonresident_count(NR_b1))+1;
	nr_freq = B2T(nonresident_count(NR_b2))+1;
	
//	ratio = (nr_freq / nr_rec) ? 0 : nr_pages;
	ratio = max_t(long, nr_pages, nr_freq/nr_rec);

	lruvec->p = min_t(unsigned long, arc_c(lruvec), lruvec->p + ratio);

//	lruvec->p += ratio;
//	if (unlikely(lruvec->p > arc_c(lruvec)))
//		lruvec->p = arc_c(lruvec);
}

static inline void __arc_p_dec(struct lruvec *lruvec, int nr_pages)
{
	/* p = max(p - max(1, nl/|B2|), 0) */
	unsigned long ratio;
	long nr_rec, nr_freq;
	
	nr_rec = B2T(nonresident_count(NR_b1))+1;
	nr_freq = B2T(nonresident_count(NR_b2))+1;

	ratio = max_t(long, nr_pages, nr_rec/nr_freq);

//	ratio = (nr_rec / nr_freq) ? 0 : nr_pages;

	lruvec->p = max_t(long, 0, lruvec->p - ratio);

//	if (lruvec->p >= ratio) lruvec->p -= ratio;
//	else lruvec->p = 0UL;
}

/*
static inline void __arc_p_inc(struct lruvec *lruvec, int nr_pages)
{
	long nr_rec, nr_freq;
	
	nr_rec = B2T(nonresident_count(NR_b1))+1;
	nr_freq = B2T(nonresident_count(NR_b2))+1;

	lruvec->p = MIN(lruvec->c, (lruvec->p + MAX(nr_freq/nr_rec, nr_pages)));	
//	lruvec->p = MIN(lruvec->c, lruvec->p + 1);	
	printk(KERN_ERR "Increase p in arc by %ld, nr_rec %ld, nr_freq %ld, nr_pages %d\n", MAX(nr_freq/nr_rec, nr_pages), nr_rec, nr_freq, nr_pages);
}

static inline void __arc_p_dec(struct lruvec *lruvec, int nr_pages)
{
	long nr_rec, nr_freq;
	
	nr_rec = B2T(nonresident_count(NR_b1))+1;
	nr_freq = B2T(nonresident_count(NR_b2))+1;

//	lruvec-> p = MAX(0, lruvec->p -1);

	lruvec->p = MAX(0, (lruvec->p-MAX(nr_rec/nr_freq, nr_pages)) );	
	printk(KERN_ERR "Dec p in arc by %ld, nr_rec %ld, nr_freq %ld, nr_pages %d\n", MAX(nr_rec/nr_freq, nr_pages), nr_rec, nr_freq, nr_pages);
}
*/
/*
static unsigned long list_count(struct list_head *list, int PG_flag, int result)
{
	unsigned long nr = 0;
	struct page *page;
	list_for_each_entry(page, list, lru) {
		if (!!test_bit(PG_flag, &(page)->flags) == result)	//Quan,  determine state or flag
			++nr;
	}
	return nr;
}
*/

static void __validate_zone(struct lruvec *lruvec)
{
#if 0
	int nr_mru = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE);
	int nr_mfu = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1);
	int bug = 0;
	unsigned long cnt0 = list_count(&(lruvec->lists[ARC_MRU]), PG_lru, 0);
	unsigned long cnt1 = list_count(&(lruvec->lists[ARC_MRU]), PG_lru, 1);

	if (cnt1 != nr_mru) {
		printk(KERN_ERR KERN_ERR "__validate_zone: MRU: %lu,%lu,%lu\n", cnt0, cnt1, 
			nr_mru);
		bug = 1;
	}

	cnt0 = list_count(&(lruvec->lists[ARC_MRU]), PG_lru, 0);
	cnt1 = list_count(&(lruvec->lists[ARC_MFU]), PG_lru, 1);
	if (cnt1 != nr_mfu || bug) {
		printk(KERN_ERR KERN_ERR "__validate_zone: T2: %lu,%lu,%lu\n", cnt0, cnt1, nr_mfu);
		bug = 1;
	}

	if (bug) {
		BUG();
	}
#endif
}


#if 0
//static DEFINE_PER_CPU(struct pagevec, arc_add_pvecs) = { 0, };
static DEFINE_PER_CPU(struct pagevec, arc_add_pvecs);

//add page into the to-be-released pagevec
void page_replace_add(struct page *page)
{
	struct pagevec *pvec = &get_cpu_var(arc_add_pvecs);
	
	printk(KERN_ERR "page replace add arc\n");
	page_cache_get(page);
	if (!pagevec_add(pvec, page))
		__pagevec_page_replace_add(pvec);
	put_cpu_var(arc_add_pvecs);
}

void __page_replace_add_drain_arc(unsigned int cpu)
{
	struct pagevec *pvec = &per_cpu(arc_add_pvecs, cpu);

	if (pagevec_count(pvec))
		__pagevec_page_replace_add(pvec);
}

#ifdef CONFIG_NUMA
static void drain_per_cpu(void *dummy)
{
	page_replace_add_drain();
}

/*
 * Returns 0 for success
 */
static int page_replace_add_drain_all_arc(void)
{
//	return schedule_on_each_cpu(drain_per_cpu, NULL);
	return schedule_on_each_cpu(drain_per_cpu);
}

#else

/*
 * Returns 0 for success
 */
static int page_replace_add_drain_all_arc(void)
{
	page_replace_add_drain();
	return 0;
}
#endif


static inline void pagevec_page_replace_add_arc(struct pagevec *pvec)
{
	if (pagevec_count(pvec))
		__pagevec_page_replace_add(pvec);
}

static inline void page_replace_add_drain_arc(void)
{
	__page_replace_add_drain_arc(get_cpu());
	put_cpu();
}

#endif

static void add_page_to_list(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, get_arc(lru), nr_pages);
	list_add(&page->lru, get_arc_list(lruvec, lru));
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + get_arc(lru),
				nr_pages);
}

static void del_page_from_list(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, get_arc(lru), -nr_pages);
	list_del(&page->lru);
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + get_arc(lru),
				-nr_pages);
}

#ifdef CONFIG_MIGRATION
/*
 * Isolate one page from the LRU lists and put it on the
 * indicated list with elevated refcount.
 *
 * Result:
 *  0 = page not on LRU list
 *  1 = page removed from LRU list and added to the specified list.
 */
//static int isolate_arc(struct page *page, struct lruvec *lruvec)
static void isolate_arc(struct page *page, struct lruvec *lruvec)
//struct page *page)
{
//	printk(KERN_ERR "isolate arc\n");

	get_page(page);
	ClearPageLRU(page);

	if (PageMRU(page)) del_page_from_list(page, lruvec, ARC_MRU);
	else 	del_page_from_list(page, lruvec, ARC_MFU);
}

#endif

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
		printk(KERN_ERR "Bug in __page_release\n");
		BUG();
	}

	SetPageLRU(page);

	if (PageMRU(page)) {
		__mod_zone_page_state(lruvec_zone(lruvec), 
				NR_LRU_BASE, nr_pages);
	} else {
		__mod_zone_page_state(lruvec_zone(lruvec), 
					NR_LRU_BASE+1, nr_pages);
//		printk(KERN_ERR "page release in arc MFU %lu\n", lruvec->nr_MFU);
	}
	
//	delete_from_history(page, lruvec->zone->history, HISTORY_EVICTABLE);

	if (!pagevec_add(pvec, page)) {
		spin_unlock_irq(&lruvec->zone->lru_lock);
//		if (buffer_heads_over_limit)
//			pagevec_strip(pvec);
		__pagevec_release(pvec);
		spin_lock_irq(&lruvec->zone->lru_lock);
	}
}

#if 1
//static void page_replace_reinsert(struct lruvec *lruvec, struct list_head *page_list)
static void putback_page_arc(struct page *page)
{
	struct zone *zone = page_zone(page);
	struct pagevec pvec;
	
	struct lruvec *lruvec = zone->lruvecs[PAGE_RECLAIM_ARC];
	pagevec_init(&pvec, 1);
	printk(KERN_ERR "putback page in arc\n");
/*
	list_for_each_entry_safe(page, page2, page_list, lru) {
		struct zone *pagezone = lruvec->zone;

		if (pagezone != zone) {
			if (zone)
				spin_unlock_irq(&zone->lru_lock);
			zone = pagezone;
			spin_lock_irq(&zone->lru_lock);
		}
*/
		if (PageMRU(page)) {
			list_move(&page->lru, &lruvec->lists[ARC_MRU]);
			dec_zone_page_state(page, NR_ISOLATED_ANON);
		} else {
			list_move(&page->lru, &lruvec->lists[ARC_MFU]);
			dec_zone_page_state(page, NR_ISOLATED_ANON+1);
		}

		__page_release(lruvec, page, &pvec);
//	}

//	if (zone)
//		spin_unlock_irq(&zone->lru_lock);
	pagevec_release(&pvec);
}
#endif

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
		isolate_mode_t mode, enum arc_list lru) 
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
		page = lru_to_page_arc(src);
		prefetchw_prev_lru_page_arc(page, src, flags);

		VM_BUG_ON(!PageLRU(page));
		
//		if (!PageLRU(page)) {
//			__ClearPageLRU(page);		BUG();
//		}

		if(lru == ARC_MRU) SetPageMRU(page);
		else if (lru == ARC_MFU) ClearPageMRU(page);

		switch (__isolate_lru_page(page, mode)) {
			case 0:
				nr_pages = hpage_nr_pages(page);
				mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
				list_move(&page->lru, dst);
				nr_taken += nr_pages;
				break;
			case -EBUSY:
				__put_page(page);	//Not sure.
				
//				SetPageLRU(page);
				list_move(&page->lru, src);
				continue;
			default:
				BUG();
		}
	}
	
//	zone->pages_scanned += scan;
	if ( lru == ARC_MRU ) {
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+ARC_MRU, -nr_taken);
		__mod_zone_page_state(lruvec_zone(lruvec), NR_ISOLATED_ANON, nr_taken);
	} else {
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+ARC_MFU, -nr_taken);
		__mod_zone_page_state(lruvec_zone(lruvec), NR_ISOLATED_ANON + 1, nr_taken);
	}
	*nr_scanned = scan;
	return nr_taken;
}

static int arc_reclaim_MRU(struct lruvec *lruvec)
{
	int ret = 0;
	int mru = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE) > lruvec->p;
	int sat = TestClearZoneSaturated(lruvec);
	int rec = ZoneReclaimedMRU(lruvec);
	
	if(mru || (!mru && (!rec && sat))) ret = 1;
	else {
		printk(KERN_ERR "nr_lru_base: %ld, p: %ld", zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE), lruvec->p);
		ret = 0;
	}
//	if ((mru && !(rec && sat)) ||
//	    (!mru && (!rec && sat)))
//			ret = 1;

	return ret;
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
	struct zone *zone = lruvec_zone(lruvec);
	enum arc_list arc;

	isolate_mode_t isolate_mode = 0;

	lru_add_drain();

//	page_replace_add_drain();
	
	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;
	

	spin_lock_irq(&zone->lru_lock);

	if (arc_reclaim_MRU(lruvec)) {
		arc = ARC_MRU;
		SetZoneReclaimedMRU(lruvec);
	} else {
		printk(KERN_ERR "reclaim MFU pages----------------------\n");
		arc = ARC_MFU;
		ClearZoneReclaimedMRU(lruvec);
	}

	nr_taken = isolate_pages(nr_to_scan, lruvec, page_list, &nr_scan, sc,
			         isolate_mode, arc);

//	printk(KERN_ERR "page replace candidates: arc %d, nr_to_scan %ld, taken %ld\n", 
//							arc, nr_to_scan, nr_taken);

	__mod_zone_page_state(zone, NR_ISOLATED_ANON+arc, -nr_taken);
	
	if (!nr_taken) {
		if (arc == ARC_MRU) {
			arc = ARC_MFU;
			ClearZoneReclaimedMRU(lruvec);
		} else {
			arc = ARC_MRU;
			SetZoneReclaimedMRU(lruvec);
		}

		nr_taken = isolate_pages(nr_to_scan, lruvec, page_list, &nr_scan, sc,
			         isolate_mode, arc);
		printk(KERN_ERR "Fail from origin: page replace candidates: arc %d, nr_to_scan %ld, taken %ld\n", 
							arc, nr_to_scan, nr_taken);
		
		__mod_zone_page_state(zone, NR_ISOLATED_ANON+arc, -nr_taken);
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
//	unsigned long dqi = 0;
//	unsigned long dqd = 0;
//	unsigned long dsl = 0;
//	unsigned long target;
	int nr_pages;

//	int nr_MRU = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE);
//	int nr_MFU = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1);


	pagevec_init(&pvec, 1);
	spin_lock_irq(&lruvec->zone->lru_lock);

//	target = min(lruvec->p + 1UL, B2T(nonresident_count(NR_b1)));
	
//	printk(KERN_ERR "page replace reinsert zone in cart: target %ld\n", target);

	while (!list_empty(page_list)) {
		struct page * page = lru_to_page_arc(page_list);
		prefetchw_prev_lru_page_arc(page, page_list, flags);
	
		nr_pages = hpage_nr_pages(page);

		if(PageMRU(page)) {
			TestClearPageReferenced(page);
	
			list_move(&page->lru, &(lruvec->lists[ARC_MRU]));
		} else {
			ClearPageMRU(page);
			list_move(&page->lru, &(lruvec->lists[ARC_MFU]));
		}

#if 0
		if (CART_PageT1(page)) { /* T1 */
			if (TestClearPageReferenced(page)) {
				if (!CART_PageLongTerm(page) &&
				    (nr_T1 - dqd + dqi) >= target) {
					CART_SetPageLongTerm(page);
					dsl += nr_pages;
				}
				list_move( &page->lru, &(lruvec->lists[CART_T1]) );
			} else if (CART_PageLongTerm(page)) {
				CART_ClearPageT1(page);
				dqd += nr_pages;
				list_move( &page->lru, &(lruvec->lists[CART_T2]) );
			} else {
				/* should have been reclaimed or was PG_new */
				list_move( &page->lru, &(lruvec->lists[CART_T1]) );
			}
		} else { /* T2 */
			if (TestClearPageReferenced(page)) {
				CART_SetPageT1(page);
				dqi += nr_pages;
				list_move( &page->lru, &(lruvec->lists[CART_T1]) );
			} else {
				/* should have been reclaimed */
				CART_ClearPageT1(page);
				list_move( &page->lru, &(lruvec->lists[CART_T2]) );
			}
		}
#endif
		__page_release(lruvec, page, &pvec);
	}

	if (!nr_freed) SetZoneSaturated(lruvec);

//	if (dqi > dqd)
//		__cart_q_inc(lruvec, dqi - dqd);
//	else
//		__cart_q_dec(lruvec, dqd - dqi);

	spin_unlock_irq(&lruvec->zone->lru_lock);
	pagevec_release(&pvec);
}
/*
///To hacking!!! Quan
static void page_replace_reinsert_zone(struct lruvec *lruvec, 
										struct list_head *page_list,
										int nr_freed)
{
	struct pagevec pvec;
	int nr_pages;
	
	struct zone * zone = lruvec_zone(lruvec);

//	int nr_MRU = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE);
//	int nr_MFU = zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1);

	printk(KERN_ERR "page replace reinsert zone: nr_freed %d\n", nr_freed);
	
	pagevec_init(&pvec, 1);
	spin_lock_irq(&zone->lru_lock);
//	target = min(lruvec->nr_p + 1UL, B2T(nonresident_count(NR_b1)));

	while (!list_empty(page_list)) {
		struct page * page = lru_to_page_arc(page_list);
		prefetchw_prev_lru_page_arc(page, page_list, flags);
	
		nr_pages = hpage_nr_pages(page);

		if(ARC_PageMRU(page)) {
			TestClearPageReferenced(page);

			list_move(&page->lru, &(lruvec->lists[ARC_MRU]));
//			__mod_zone_page_state(zone, NR_LRU_BASE, nr_pages);
//			lruvec->nr_MRU += nr_pages;
		} else if (ARC_PageMFU(page)){
			list_move(&page->lru, &(lruvec->lists[ARC_MFU]));
//			__mod_zone_page_state(zone, NR_LRU_BASE+1, nr_pages);
//			lruvec->nr_MFU += nr_pages;
		}
		
		__page_release(lruvec, page, &pvec);
	}
	
	if (!nr_freed) SetZoneSaturated(lruvec);

	spin_unlock_irq(&zone->lru_lock);
	pagevec_release(&pvec);
}*/

/*
void __page_replace_rotate_reclaimable(struct lruvec *lruvec, struct page *page)
{
	int nr_pages = hpage_nr_pages(page);

	if (PageLRU(page)) {
		if (arc_PageLongTerm(page)) {
			if (arc_TestClearPageT1(page)) {
				__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE, -nr_pages);
				__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE+1, nr_pages);
				__arc_q_dec(lruvec, nr_pages);
//				--lruvec->nr_T1;
//				++lruvec->nr_T2;
//				__arc_q_dec(lruvec, 1);
			}
			list_move_tail( &page->lru, &(lruvec->lists[arc_T2]) );
		} else {
			if (!arc_PageT1(page))
				BUG();
			list_move_tail( &page->lru, &(lruvec->lists[arc_T1]) );
		}
	}
}
*/

void nonres_remember_arc(struct zone *zone, struct page *page)
{
	struct lruvec *lruvec = zone->lruvecs[PAGE_RECLAIM_ARC];
	int target_list = PageMRU(page) ? NR_b1 : NR_b2;
	int evict_list = (nonresident_count(NR_b1) > arc_q())
		? NR_b1 : NR_b2;
	
	printk(KERN_ERR "page replace remember arc: target %d, evict %d, NR b1 %ld, NR b2 %ld, MRU count %lu, MFU count %lu, p is %ld, q is %ld\n", 
			target_list, evict_list, nonresident_count(NR_b1), 
			nonresident_count(NR_b2), 
			zone_page_state(zone, NR_LRU_BASE+ARC_MRU),
			zone_page_state(zone, NR_LRU_BASE+ARC_MFU),
			lruvec->p,
			arc_q()
			);
	
	nonresident_put(page_mapping(page), page_index(page),
			target_list, evict_list);
	
}

static void nonres_forget_arc(struct address_space *mapping, unsigned long index)
{
	nonresident_get(mapping, index);
}

#define K(x) ((x) << (PAGE_SHIFT-10))

/*
void page_replace_show(struct zone *zone)
{
	printk(KERN_ERR "%s"
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

struct list_head* get_test_list_arc(struct lruvec *lruvec, int lru)
{
	return &lruvec->lists[lru];
}

void print_lruvec_arc(struct zone *zone)
{
	printk(KERN_ERR "arc: Helper Function.\n");
}

void get_scan_count_arc(struct lruvec *lruvec, struct scan_control *sc,
				unsigned long *nr, bool force_scan) 
{
	
	unsigned long nr_scan;

	nr_scan = get_arc_size(lruvec, ARC_MRU) +
				get_arc_size(lruvec, ARC_MFU);

	nr_scan >>= sc->priority;
	nr_scan ++;

	nr[ARC_MRU] = nr_scan;
	nr[ARC_MFU] = 0;

/*
	unsigned long nr_scan = (__page_replace_nr_scan(lruvec) >> sc->priority) + 1;
	unsigned long t1;
	unsigned long t2;
	unsigned long p_tmp = lruvec->p;
	unsigned long nr_1 = 0;
	unsigned long nr_2 = 0;

//	t1 = lruvec->nr_MRU;
//	t2 = lruvec->nr_MFU;
	t1 = get_arc_size(lruvec, ARC_MRU);	
	t2 = get_arc_size(lruvec, ARC_MFU);	

	if(nr_scan < SWAP_CLUSTER_MAX)	nr_scan = SWAP_CLUSTER_MAX;

	while(nr_scan >= SWAP_CLUSTER_MAX) {
		if (t1 >= p_tmp) {

			nr_1 += SWAP_CLUSTER_MAX;
			t1 -= SWAP_CLUSTER_MAX;	

//			if(p_tmp > SWAP_CLUSTER_MAX)
//				p_tmp -= SWAP_CLUSTER_MAX;	//not precise, an approximation

		} else if (t2 > SWAP_CLUSTER_MAX) {
			nr_2 += SWAP_CLUSTER_MAX;
			t2 -= SWAP_CLUSTER_MAX;
//			p_tmp += SWAP_CLUSTER_MAX;	//not precise, an approximation
		}

		nr_scan -= SWAP_CLUSTER_MAX;
	}
	
	nr[ARC_MRU] = nr_1;
	nr[ARC_MFU] = nr_2;
*/
}

static int too_many_isolated(struct zone *zone, int file,
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
//		inactive = zone_page_state(zone, NR_INACTIVE_ANON);
		inactive = zone_page_state(zone, NR_LRU_BASE);
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
		printk(KERN_ERR "isolated: %ld, inactive %ld, is MFU: %d\n", isolated, inactive, file);

	return isolated > inactive;
}

bool too_many_isolated_comapction_arc(struct zone *zone)
{
	unsigned long all, isolated;

	all = zone_page_state(zone, NR_LRU_BASE)+zone_page_state(zone, NR_LRU_BASE+1);
	isolated = zone_page_state(zone, NR_ISOLATED_ANON)+zone_page_state(zone, NR_ISOLATED_ANON+1);

	return isolated > all;
}

inline int is_MFU_arc(enum arc_list arc)
{
	return (arc == ARC_MFU);
}

noinline_for_stack unsigned long 
shrink_list_arc (unsigned long nr_to_scan, struct lruvec *lruvec,
		struct scan_control *sc, enum arc_list arc) 
{
	LIST_HEAD(page_list);
	unsigned long nr_reclaimed = 0;
	unsigned long nr_taken;
	unsigned long nr_dirty = 0;
	unsigned long nr_writeback = 0;

	int is_MFU = is_MFU_arc(arc);
	struct zone *zone = lruvec_zone(lruvec);
	
//	printk(KERN_ERR "shrink list arc: nr_to_scan %ld, arc %d\n", nr_to_scan, arc);
	
	while (unlikely(too_many_isolated(zone, is_MFU, sc))) {
		printk(KERN_ERR "shrink list arc: too many isolated\n");
		congestion_wait(BLK_RW_ASYNC, HZ/10);

		/* We are about to die and free our memory. Return now. */
		if (fatal_signal_pending(current)) {
			printk(KERN_ERR "shrink list arc: fatal_signal_pending\n");
			return SWAP_CLUSTER_MAX;
		}
	}

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
	
//	__mod_zone_page_state(zone, NR_ISOLATED_ANON+is_MFU, -nr_taken);

	spin_unlock_irq(&zone->lru_lock);

	page_replace_reinsert_zone(lruvec, &page_list, nr_reclaimed);
	
	if (nr_writeback && nr_writeback >=
			(nr_taken >> (DEF_PRIORITY - sc->priority)))
		wait_iff_congested(zone, BLK_RW_ASYNC, HZ/10);
	
	throttle_vm_writeout(sc->gfp_mask);

	return nr_reclaimed;
}

static int shrink_lruvec_arc(struct lruvec *lruvec, struct scan_control *sc,
					unsigned long *nr)
{
	unsigned long nr_to_scan;
//	enum arc_list arc;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;

//	printk(KERN_ERR "shrink lruvec arc: %ld, %ld\n", nr[ARC_MRU], nr[ARC_MFU]);
	
	while (nr[ARC_MRU] >= SWAP_CLUSTER_MAX) {
		nr_to_scan = min_t(unsigned long, nr[ARC_MRU], SWAP_CLUSTER_MAX);

		nr[ARC_MRU] -= nr_to_scan;
	
		nr_reclaimed +=shrink_list_arc(nr_to_scan, lruvec, sc, ARC_MRU);
		
		if(nr_reclaimed >= nr_to_reclaim && sc->priority < DEF_PRIORITY)
			break;
	}
/*
	while (nr[ARC_MRU] || nr[ARC_MFU]) {
		for_each_evictable_arc(arc) {
			if(nr[arc]) {
				nr_to_scan = min_t(unsigned long, nr[arc], SWAP_CLUSTER_MAX);
				nr[arc] -= nr_to_scan;

				nr_reclaimed +=shrink_list_arc(nr_to_scan, lruvec, sc, arc);
			}
		}

		if(nr_reclaimed >= nr_to_reclaim && sc->priority < DEF_PRIORITY)
			break;
	}
*/

	return nr_reclaimed;
}

void balance_lruvec_arc(struct lruvec *lruvec, struct scan_control *sc)
{

}

bool should_continue_reclaim_arc(struct lruvec *lruvec, 
								unsigned long nr_reclaimed,
								unsigned long nr_scanned,
								struct scan_control *sc)
{
	unsigned long reclaimable_pages;
	unsigned long pages_for_compaction = (2UL << sc->order);

	reclaimable_pages = zone_page_state(lruvec_zone(lruvec), ARC_MRU)
		+ zone_page_state(lruvec_zone(lruvec), ARC_MFU);

	if (sc->nr_reclaimed < pages_for_compaction && 
	    reclaimable_pages > pages_for_compaction)
		return true;

	printk(KERN_ERR "should_continue_reclaim_arc():pages_for_compaction:%lu\n", 
						pages_for_compaction);

	return false;
}

unsigned long global_reclaimable_pages_arc(void)
{
	return global_page_state(NR_LRU_BASE);
}

unsigned long zone_reclaimable_pages_arc(struct zone *zone)
{
	return zone_page_state(zone, NR_LRU_BASE);
}



void activate_page_arc(struct page *page, struct lruvec *lruvec)
{
	/* Move the page to MFU if the page is accessed a second time, which means
	 * it's activated. */

	int target = PageMRU(page)? 0 : 1;

	del_page_from_list(page, lruvec, target);
	SetPageActive(page);
	ClearPageMRU(page);
	add_page_to_list(page, lruvec, ARC_MFU);
	
//	__arc_q_dec(lruvec, nr_pages);

//	printk(KERN_ERR "Acitve page from %d active page \n", target);
}

/*
 * Mark a page as having seen activity.
 *
 * inactive,unreferenced	->	inactive,referenced
 * inactive,referenced		->	active,unreferenced
 * active,unreferenced		->	active,referenced
 */
void page_accessed_arc(struct page *page)
{
//	struct zone *zone = page_zone(page);
	if (!PageActive(page) && !PageUnevictable(page) &&
			PageReferenced(page) && PageLRU(page)) {
		activate_page(page);
//		activate_page_arc(page, zone->lruvecs[PAGE_RECLAIM_ARC]);
//		ClearPageReferenced(page);
	} else if (!PageReferenced(page)) {
		SetPageReferenced(page);
	}
}

void deactivate_page_arc(struct page *page, struct lruvec *lruvec,
				 bool page_writeback_or_dirty)
{
	/* Don't do anything if the page has been inactive for a while.
	 * It's FIFO. */
}

void update_reclaim_statistics_arc(struct lruvec *lruvec, int type,
					   int rotated)
{
	/* We don't account for page statistics for now. */
}


void add_page_to_list_arc(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	add_page_to_list(page, lruvec, lru);
}

void del_page_from_list_arc(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	del_page_from_list(page, lruvec, lru);
}

void add_page_unevictable_arc(struct page *page, struct lruvec *lruvec)
{
	add_page_to_list(page, lruvec, ARC_UNEVICTABLE);
}

void add_page_arc(struct page *page, struct lruvec *lruvec, int lru)
{
	//added by quan

	int nr_pages;
	unsigned int rflags;
	/*
	 * Note: we could give hints to the insertion process using the LRU
	 * specific PG_flags like: PG_t1, PG_longterm and PG_referenced.
	 */

	if(lru % 2 == 0) {
		SetPageMRU(page);
	} else {
		ClearPageMRU(page);
	}

	if(!PageLRU(page)) {
		printk(KERN_ERR "the page is not in LRU\n");
		BUG_ON(!PageLRU(page));
//		SetPageLRU(page);
	}

	nr_pages = hpage_nr_pages(page);

	rflags = nonresident_get(page_mapping(page), page_index(page));

	if(rflags & NR_found) {
		ClearPageMRU(page);
		rflags &= NR_listid;

		if(rflags == NR_b1) {
			__arc_p_inc(lruvec, nr_pages);
//			__arc_q_dec(lruvec, nr_pages);
		} else if (rflags == NR_b2) {
			__arc_p_dec(lruvec, nr_pages);
			__arc_q_inc(lruvec, nr_pages);
		}
	} else {
		SetPageMRU(page);
	}

	if(PageMRU(page)) {
		add_page_to_list_arc(page, lruvec, ARC_MRU);
	} else {
		add_page_to_list_arc(page, lruvec, ARC_MFU);
	}

	__validate_zone(lruvec);
}

void release_page_arc(struct page *page, struct lruvec *lruvec, int lru,
			     bool batch_release)
{
	int is_mru = PageMRU(page);
	
	if(is_mru)
		del_page_from_list_arc(page, lruvec, ARC_MRU);
	else 
		del_page_from_list_arc(page, lruvec, ARC_MFU);
//	printk(KERN_ERR "Release_page arc: del page from list %d %d\n", is_mru, is_mfu);

//del_page_from_list_arc(page, lruvec, lru);
	
	page_replace_clear_state(page);

	if(is_mru)	ClearPageMRU(page);
}


void rotate_inactive_page_arc(struct page *page, struct lruvec *lruvec)
{
	//To add
	if(PageMRU(page))
		list_move_tail(&page->lru, &(lruvec->lists[ARC_MRU]));
	else 
		list_move_tail(&page->lru, &(lruvec->lists[ARC_MFU]));
//	list_move_tail(&page->lru, &lruvec->lists[FIFO_EVICTABLE]);
}

void reset_zone_vmstat_arc(struct lruvec *lruvec, struct zone *zone,
				   bool evictable)
{
	enum arc_list arc;
	int index, nr_pages, i=0;
	struct page *page;
//	int lru = evictable ? ARC_BASE : ARC_UNEVICTABLE;

	if(evictable) {
		for_each_evictable_arc(arc) {
			index = NR_LRU_BASE + arc;
			
			nr_pages = zone_page_state(zone, index);
			__mod_zone_page_state(zone, index, -nr_pages);
			mem_cgroup_update_lru_size(lruvec, arc, -nr_pages);
			printk(KERN_ERR "EVIC: %d, num of pages %d\n", arc, nr_pages);	
			
			list_for_each_entry(page, &(lruvec->lists[arc]), lru) {
			
				if(!PageUnevictable(page) && PageLRU(page)) {
//					list_del(&page->lru);
					add_to_history(page,  zone->history, HISTORY_EVICTABLE);
				}
//				nr_evictable ++;
			}
		}

		nr_pages = zone_page_state(zone, NR_ISOLATED_ANON);
		__mod_zone_page_state(zone, NR_ISOLATED_ANON, -nr_pages);
		nr_pages = zone_page_state(zone, NR_ISOLATED_FILE);
		__mod_zone_page_state(zone, NR_ISOLATED_FILE, -nr_pages);
	} else {
		index = NR_LRU_BASE + ARC_UNEVICTABLE;
		
		nr_pages = zone_page_state(zone, index);
		__mod_zone_page_state(zone, index, -nr_pages);
		mem_cgroup_update_lru_size(lruvec, ARC_UNEVICTABLE, -nr_pages);
		
		list_for_each_entry(page, &(lruvec->lists[ARC_UNEVICTABLE]), lru) {
			if(PageUnevictable(page)) {
				add_to_history(page,  zone->history, HISTORY_UNEVICTABLE);
			}
		}
		
		printk(KERN_ERR "UNEVIC: add %d pages, num of pages %d\n", i, nr_pages);	
	}
	
	lruvec->p = 0;

	lruvec->flags = 0;

	for_each_cpu(i, cpu_possible_mask)
		per_cpu(arc_nr_q, i) = 0;

//Reset the nonresident page list
//
/*
	index = NR_LRU_BASE + lru;
	nr_pages = zone_page_state(zone, index);
	__mod_zone_page_state(zone, index, -nr_pages);
	mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
*/
}

static int activate_arc(struct page *page)
{
	if (!TestClearPageNew(page)) {
		SetPageReferenced(page);
		SetPageActive(page);
		ClearPageMRU(page);
		return 1;
	}
	
	return 0;
	
//	VM_BUG_ON(PageActive(page));
//	SetPageActive(page);
//	return 1;
}

static int page_check_references_arc(struct page *page, struct scan_control *sc)
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
	
//	if((PageT1(page) && PageLongTerm(page)) ||
//			(!PageT1(page) && !PageLongTerm(page)))
//		return PAGEREF_KEEP;

	return PAGEREF_RECLAIM;

#if 0
	if (referenced_ptes) {
		if (PageSwapBacked(page))
			return PAGEREF_ACTIVATE;
		/*
		 * All mapped pages start out with page table
		 * references from the instantiating fault, so we need
		 * to look twice if a mapped file page is used more
		 * than once.
		 *
		 * Mark it and spare it for another trip around the
		 * inactive list.  Another page table reference will
		 * lead to its activation.
		 *
		 * Note: the mark is set for activated pages as well
		 * so that recently deactivated but used pages are
		 * quickly recovered.
		 */
		SetPageReferenced(page);

		if (referenced_page || referenced_ptes > 1)
			return PAGEREF_ACTIVATE;

		/*
		 * Activate file-backed executable pages after first usage.
		 */
		if (vm_flags & VM_EXEC)
			return PAGEREF_ACTIVATE;

		return PAGEREF_KEEP;
	}

	/* Reclaim if clean, defer dirty pages to writeback */
	if (referenced_page && !PageSwapBacked(page))
		return PAGEREF_RECLAIM_CLEAN;

	return PAGEREF_RECLAIM;
#endif
}

static inline void hint_use_once_arc(struct page *page) {
	if(PageLRU(page))
		BUG();
	SetPageNew(page);
}

const struct page_reclaim_policy arc_page_reclaim_policy =
{
	/* Initialize the structures. */
	.init_lruvec = init_lruvec_arc,

	/* Decide which pages to reclaim and actually do the reclaiming. */
	.get_scan_count = get_scan_count_arc,
	.shrink_lruvec = shrink_lruvec_arc,
	.balance_lruvec = balance_lruvec_arc,

	/* Helpers used when deciding which pages to reclaim and compact.*/
	.should_continue_reclaim = should_continue_reclaim_arc,
	.zone_reclaimable_pages = zone_reclaimable_pages_arc,
	.global_reclaimable_pages = global_reclaimable_pages_arc,
	.too_many_isolated_compaction = too_many_isolated_comapction_arc,

	/* Capture activity and statistics */
	.page_accessed = page_accessed_arc,
	.activate_page = activate_page_arc,
	.deactivate_page = deactivate_page_arc,
	.update_reclaim_statistics = update_reclaim_statistics_arc,

	/* Add/remove pages from the lists. */
	.add_page_to_list = add_page_to_list_arc,
	.del_page_from_list = del_page_from_list_arc,
	.add_page = add_page_arc,
	.release_page = release_page_arc,
	.add_page_unevictable = add_page_unevictable_arc,

	/* Helpers used for specific scenarios. */
	.rotate_inactive_page = rotate_inactive_page_arc,
	.get_lruvec_list = get_arc_list,
	.reset_zone_vmstat = reset_zone_vmstat_arc,

	/* For testing purposes */
	.get_list = get_test_list_arc,
	.print_lruvec = print_lruvec_arc,

	/*For nonresident lists*/
	.nonres_remember = nonres_remember_arc,
	.nonres_forget = nonres_forget_arc,
	.isolate = isolate_arc,
	.putback_page = putback_page_arc,

	/* New */
	.activate = activate_arc,
	.page_check_references = page_check_references_arc,
//	.hint_use_once = hint_use_once_arc,
};
EXPORT_SYMBOL(arc_page_reclaim_policy);

#endif	/*CONFIG_RECLAIM_POLICY*/
