#ifdef CONFIG_RECLAIM_POLICY
/*
 * Implements the CLOCK-Pro page replacement
 * algorithm.
 * Methods from already defined clock-pro.c
 * 1. pgrep_init
 * 2. pgrep_init_zone
 * 3. __pgrep_add
 * 4. pgrep_add
 * 5. __pgrep_add_drain
 * 6. __page_release
 * 7. pgrep_reinsert
 * 8. __pgrep_get_candidates
 * 9. pgrep_put_candidates
 * 10. pgrep_remember
 * 11. pgrep_forget
 * 12. pgrep_show
 * 13. pgrep_zoneinfo
 * 14. __pgrep_counts
 * TODO: 1. shrink_page_list - activate the page, modify its flags
 *       2. pgrep_copy_state and pgrep_clear_state - mm/migrate.c
 *       3.
 */
#include <linux/bootmem.h>
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

#include <linux/nonresident.h>

#define lru_to_page(_head) (list_entry((_head)->prev, struct page, lru))

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

#define PG_hot		PG_reclaim1
#define PG_test		PG_reclaim2

#define PageHot(page)		test_bit(PG_hot, &(page)->flags)
#define SetPageHot(page)	set_bit(PG_hot, &(page)->flags)
#define ClearPageHot(page)	clear_bit(PG_hot, &(page)->flags)
#define TestClearPageHot(page)	test_and_clear_bit(PG_hot, &(page)->flags)
#define TestSetPageHot(page)	test_and_set_bit(PG_hot, &(page)->flags)

#define PageTest(page)		test_bit(PG_test, &(page)->flags)
#define SetPageTest(page)	set_bit(PG_test, &(page)->flags)
#define ClearPageTest(page)	clear_bit(PG_test, &(page)->flags)
#define TestClearPageTest(page)	test_and_clear_bit(PG_test, &(page)->flags)

#define is_file_lru(lru) (lru == CLOCK_PRO_FILE_COLD || lru == CLOCK_PRO_FILE_HOT)

#define CLOCK_PRO_BASE 0
enum clock_pro_list {
	CLOCK_PRO_ANON_COLD = CLOCK_PRO_BASE,
	CLOCK_PRO_ANON_HOT,
	CLOCK_PRO_FILE_COLD,
	CLOCK_PRO_FILE_HOT,
	CLOCK_PRO_UNEVICTABLE,
	NR_CLOCK_PRO_LISTS
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
	struct list_head lists[NR_CLOCK_PRO_LISTS];
	unsigned long		nr_cold_target; // we may leave it
	unsigned long		nr_nonresident_scale; // we may leave it fixed
};

/*
 * The nonresident code can be seen as a single handed clock that
 * lacks the ability to remove tail pages. However it can report the
 * distance to the head.
 *
 * What is done is to set a threshold that cuts of the clock tail.
 */
static DEFINE_PER_CPU(unsigned long, nonres_cutoff) = 0;

/* 
 * Keep track of the number of nonresident pages tracked.
 * This is used to scale the hand hot vs nonres hand rotation.
 */
static DEFINE_PER_CPU(unsigned long, nonres_kount) = 0;

/*
 * number of truncated nonresident entries relative to the total
 * number of possible nonresident entries.
 */
static inline unsigned long __nonres_cutoff(void)
{
	return __sum_cpu_var(unsigned long, nonres_cutoff);
}

/*
 * number of actual nonresident entries (valid + truncated)
 */
static inline unsigned long __nonres_count(void)
{
	return __sum_cpu_var(unsigned long, nonres_kount);
}

/*
 * the nonresident distance threshold:
 *   max nr - tail
 */
static inline unsigned long nonres_threshold(void)
{
	return nonresident_total() - __nonres_cutoff();
}

/*
 * inc/dec the cutoff
 */
static void __nonres_cutoff_inc(unsigned long dt)
{
	unsigned long count = __nonres_count();
	unsigned long cutoff = __nonres_cutoff();
	if (cutoff < count - dt)
		__get_cpu_var(nonres_cutoff) += dt;
	else
		__get_cpu_var(nonres_cutoff) += count - cutoff;
}

static void __nonres_cutoff_dec(unsigned long dt)
{
	unsigned long cutoff = __nonres_cutoff();
	if (cutoff > dt)
		__get_cpu_var(nonres_cutoff) -= dt;
	else
		__get_cpu_var(nonres_cutoff) -= cutoff;
}

static int get_hot_lru(int lru)
{
	switch(lru) {
		case LRU_INACTIVE_ANON:
		case LRU_ACTIVE_ANON:
			return CLOCK_PRO_ANON_HOT;
		case LRU_INACTIVE_FILE:
		case LRU_ACTIVE_FILE:
			return CLOCK_PRO_FILE_HOT;
		case LRU_UNEVICTABLE:
			return CLOCK_PRO_UNEVICTABLE;
		default:
			return CLOCK_PRO_ANON_HOT;
	}
}

static int get_cold_lru(int lru)
{
	switch(lru) {
		case LRU_INACTIVE_ANON:
		case LRU_ACTIVE_ANON:
			return CLOCK_PRO_ANON_COLD;
		case LRU_INACTIVE_FILE:
		case LRU_ACTIVE_FILE:
			return CLOCK_PRO_FILE_COLD;
		case LRU_UNEVICTABLE:
			return CLOCK_PRO_UNEVICTABLE;
		default:
			return CLOCK_PRO_ANON_COLD;
	}
}

static unsigned long get_clockpro_cold(struct lruvec *lruvec)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, CLOCK_PRO_ANON_COLD) + 
			mem_cgroup_get_lru_size(lruvec, CLOCK_PRO_FILE_COLD);

	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + CLOCK_PRO_ANON_COLD) + 
		zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + CLOCK_PRO_FILE_COLD);
}

static unsigned long get_clockpro_resident(struct lruvec *lruvec)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, CLOCK_PRO_ANON_HOT) + 
			mem_cgroup_get_lru_size(lruvec, CLOCK_PRO_FILE_HOT);

	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + CLOCK_PRO_ANON_HOT) + 
		zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + CLOCK_PRO_FILE_HOT);
}

static unsigned long get_clockpro_resident_list(struct lruvec *lruvec,
						enum clock_pro_list lru)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, get_hot_lru(lru));

	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + get_hot_lru(lru));
}

static void update_cold_resident_stats(struct page *page, struct lruvec *lruvec,
				       int cold_lru, int hot_lru, int nr_pages)
{
	struct zone *zone = lruvec_zone(lruvec);
	/* Count resident. */
	mem_cgroup_update_lru_size(lruvec, hot_lru, nr_pages);
	__mod_zone_page_state(zone, NR_LRU_BASE + hot_lru, nr_pages);

	if (hot_lru == LRU_UNEVICTABLE || PageHot(page))
		return;

	/* Count cold. */
	mem_cgroup_update_lru_size(lruvec, cold_lru, nr_pages);
	__mod_zone_page_state(zone, NR_LRU_BASE + cold_lru, nr_pages);
}

/*
 * Increase the cold pages target; limit it to the total number of resident
 * pages present in the current zone.
 */
static void cold_target_inc(struct lruvec *lruvec, unsigned long dct)
{
	unsigned long max = get_clockpro_resident(lruvec) -
				2 * lruvec_zone(lruvec)->watermark[WMARK_HIGH];
	if (lruvec->nr_cold_target < max - dct)
		lruvec->nr_cold_target += dct;
	else
		lruvec->nr_cold_target = max;
}

/*
 * Decrease the cold pages target; limit it to the high watermark in order
 * to always have some pages available for quick reclaim.
 */
static void cold_target_dec(struct lruvec *lruvec, unsigned long dct)
{
	unsigned long min = 2 * lruvec_zone(lruvec)->watermark[WMARK_HIGH];
	if (lruvec->nr_cold_target > min + dct)
		lruvec->nr_cold_target -= dct;
	else
		lruvec->nr_cold_target = min;
}

/*
 * Instead of a single CLOCK with two hands, two lists are used.  When the two
 * lists are laid head to tail two junction points appear, these points are the
 * hand positions.
 *
 * This approach has the advantage that there is no pointer magic associated
 * with the hands. It is impossible to remove the page a hand is pointing to.
 *
 * To allow the hands to lap each other the lists are swappable; eg.  when the
 * hands point to the same position, one of the lists has to be empty - however
 * it does not matter which list is. Hence we make sure that the hand we are
 * going to work on contains the pages.
 */
static inline
void select_list_hand(struct lruvec *lruvec, struct list_head *list, int base)
{
	if (list_empty(list)) {
		LIST_HEAD(tmp);
		list_splice_init(&lruvec->lists[base], &tmp);
		list_splice_init(&lruvec->lists[base + 1],
				 &lruvec->lists[base]);
		list_splice(&tmp, &lruvec->lists[base + 1]);
	}
}

/*
 * check if a page has an active test period associated.
 */
static int nonres_get(struct address_space *mapping, unsigned long index, int is_fault)
{
	int found = 0;
	unsigned long distance = nonresident_get(mapping, index);//, is_fault);
	if (distance != ~0UL) {
		--__get_cpu_var(nonres_kount);

		/* 
		 * If the distance is below the threshold the test period is
		 * still valid. Otherwise a tail page was found and we can
		 * decrease the the cutoff.
		 *
		 * NOTE: the cold target was adjusted when the threshold was
		 * decreased.
		 */
		found = distance < nonres_threshold();
		if (found)
			__nonres_cutoff_dec(1);
	}

	return found;
}

/*
 * store a page's test period
 */
static int nonres_put(struct address_space *mapping, unsigned long index)
{
	//nonresident_put ---> the two args are not correct maybe
	if (nonresident_put(mapping, index, 0, 0)) {
		/* 
		 * Nonresident clock eats tail due to limited size; hand test
		 * equivalent.
		 */
		__nonres_cutoff_dec(1);
		return 1;
	}

	++__get_cpu_var(nonres_kount);
	return 0;
}

/*
 * Rotate the nonresident hand by truncating the tail.
 */
static inline void nonres_rotate(unsigned long nr)
{
	__nonres_cutoff_inc(nr);
}

/*
 * Puts cold pages that have their test bit set on the non-resident lists.
 *
 * @zone: dead pages zone.
 * @page: dead page.
 */
static void nonres_remember_clock_pro(struct zone *zone, struct page *page)
{
	if (PageTest(page) && nonres_put(page_mapping(page), page_index(page)))
		cold_target_dec(zone->lruvecs[PAGE_RECLAIM_CLOCKPRO], 1);
}

static void nonres_forget_clock_pro(struct address_space *mapping, unsigned long index)
{
	nonres_get(mapping, index, 0);
}

static void add_page_to_list(struct page *page, struct lruvec *lruvec, int lru)
{
	int nr_pages = hpage_nr_pages(page);
	int found = 0;
	int hot_lru = get_hot_lru(lru);
	int cold_lru = get_cold_lru(lru);
	struct address_space *mapping = page_mapping(page);

	if (mapping)
		found = nonres_get(mapping, page_index(page), 1);

	SetPageTest(page);
	//if (PageTest(page)) {
	if (!found)
		ClearPageTest(page);
	//}

	/* Always add to COLD list. */
	list_add(&page->lru, &lruvec->lists[cold_lru]);
	update_cold_resident_stats(page, lruvec, cold_lru, hot_lru, nr_pages);
}

static void del_page_from_list(struct page *page, struct lruvec *lruvec, int lru)
{
	int nr_pages = hpage_nr_pages(page);
	int hot_lru = get_hot_lru(lru);
	int cold_lru = get_cold_lru(lru);

	list_del(&page->lru);
	update_cold_resident_stats(page, lruvec, cold_lru, hot_lru, -nr_pages);
}

/*
 * A direct reclaimer may isolate SWAP_CLUSTER_MAX pages from the LRU list and
 * then get resheduled. When there are massive number of tasks doing page
 * allocation, such sleeping direct reclaimers may keep piling up on each CPU,
 * the LRU list will go small and be scanned faster than necessary, leading to
 * unnecessary swapping, thrashing and OOM.
 */
static int too_many_isolated(struct zone *zone, int file,
			     struct scan_control *sc)
{
	unsigned long cold, isolated;

	if (current_is_kswapd())
		return 0;

	if (!global_reclaim(sc))
		return 0;

	if (file) {
		cold = zone_page_state(zone, NR_INACTIVE_FILE);
		isolated = zone_page_state(zone, NR_ISOLATED_FILE);
	} else {
		cold = zone_page_state(zone, NR_INACTIVE_ANON);
		isolated = zone_page_state(zone, NR_ISOLATED_ANON);
	}

	/*
	 * GFP_NOIO/GFP_NOFS callers are allowed to isolate more pages, so they
	 * won't get blocked by normal direct-reclaimers, forming a circular
	 * deadlock.
	 */
	if ((sc->gfp_mask & GFP_IOFS) == GFP_IOFS)
		cold >>= 3;

	return isolated > cold;
}

static noinline_for_stack void
putback_pages(struct lruvec *lruvec, struct list_head *page_list)
{
	struct zone *zone = lruvec_zone(lruvec);
	unsigned long dct = 0;
	LIST_HEAD(pages_to_free);

	/*
	 * Put back any unfreeable pages.
	 */
	while (!list_empty(page_list)) {
		struct page *page = lru_to_page(page_list);
		int lru;
		int hand;
		int hot_lru;
		int nr_pages;

		VM_BUG_ON(PageLRU(page));
		list_del(&page->lru);
		if (unlikely(!page_evictable(page))) {
			spin_unlock_irq(&zone->lru_lock);
			putback_lru_page(page);
			spin_lock_irq(&zone->lru_lock);
			continue;
		}

		lruvec = mem_cgroup_page_lruvec(page, zone);

		SetPageLRU(page);
		lru = page_lru(page);
		hot_lru = hand = get_hot_lru(lru);
		nr_pages = hpage_nr_pages(page);

		if (PageHot(page) && PageTest(page)) {
			ClearPageTest(page);
			dct += nr_pages;
			--hand; /* relocate promoted pages */
		}
		//causing problems with invalid pointer
		list_add(&page->lru, &lruvec->lists[hand]);
		update_cold_resident_stats(page, lruvec, get_cold_lru(lru),
					   hot_lru, nr_pages);

		if (put_page_testzero(page)) {
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_list(page, lruvec, lru);
			ClearPageTest(page);
			ClearPageHot(page);
			dct -= nr_pages;

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&zone->lru_lock);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&zone->lru_lock);
			} else
				list_add(&page->lru, &pages_to_free);
		}
	}

	cold_target_inc(lruvec, dct);

	/*
	 * To save our caller's stack, now use input list for pages to free.
	 */
	list_splice(&pages_to_free, page_list);
}

/*
 * zone->lru_lock is heavily contended.  Some of the functions that
 * shrink the lists perform better by taking out a batch of pages
 * and working on them outside the LRU lock.
 */
static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		isolate_mode_t mode, enum clock_pro_list lru)
{
	struct list_head *src = &lruvec->lists[lru];
	unsigned long nr_taken = 0;
	unsigned long scan;

	for (scan = 0; scan < nr_to_scan && !list_empty(src); scan++) {
		struct page *page;
		int nr_pages;

		page = lru_to_page(src);
		prefetchw_prev_lru_page(page, src, flags);

		VM_BUG_ON(!PageLRU(page));

		switch (__isolate_lru_page(page, mode)) {
		case 0:
			nr_pages = hpage_nr_pages(page);
			mem_cgroup_update_lru_size(lruvec, get_hot_lru(lru), -nr_pages);
			if (!PageHot(page)) {
				mem_cgroup_update_lru_size(lruvec, 
						get_cold_lru(lru), -nr_pages);
			}
			ClearPageActive(page);
			list_move(&page->lru, dst);
			if (!PageHot(page)) {
				__mod_zone_page_state(lruvec_zone(lruvec),
				    NR_LRU_BASE + get_cold_lru(lru), -nr_pages);
			}
			nr_taken += nr_pages;
			break;

		case -EBUSY:
			/* else it is being freed elsewhere */
			list_move(&page->lru, src);
			continue;

		default:
			BUG();
		}
	}

	*nr_scanned = scan;
	return nr_taken;
}

/*
 * We can only receive cold lru (i.e. CLOCK_PRO_ANON_COLD, CLOCK_PRO_FILE_COLD)
 */
static noinline_for_stack unsigned long
shrink_cold_list(unsigned long nr_to_scan, struct lruvec *lruvec,
		 struct scan_control *sc, enum clock_pro_list lru)
{
	LIST_HEAD(page_list);
	unsigned long nr_scanned;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_taken;
	unsigned long nr_dirty = 0;
	unsigned long nr_writeback = 0;
	isolate_mode_t isolate_mode = 0;
	int file = is_file_lru(lru);
	struct zone *zone = lruvec_zone(lruvec);

	while (unlikely(too_many_isolated(zone, file, sc))) {
		congestion_wait(BLK_RW_ASYNC, HZ/10);

		/* We are about to die and free our memory. Return now. */
		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}

	lru_add_drain();

	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	spin_lock_irq(&zone->lru_lock);

	select_list_hand(lruvec, &lruvec->lists[lru], lru);
	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &page_list,
				     &nr_scanned, sc, isolate_mode, lru);

	__mod_zone_page_state(zone, NR_LRU_BASE + get_hot_lru(lru), -nr_taken);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, nr_taken);

	if (global_reclaim(sc)) {
		zone->pages_scanned += nr_scanned;
		if (current_is_kswapd())
			__count_zone_vm_events(PGSCAN_KSWAPD, zone, nr_scanned);
		else
			__count_zone_vm_events(PGSCAN_DIRECT, zone, nr_scanned);
	}
	spin_unlock_irq(&zone->lru_lock);

	if (nr_taken == 0)
		return 0;

	nr_reclaimed = shrink_page_list(&page_list, zone, sc, TTU_UNMAP,
					&nr_dirty, &nr_writeback, false);

	spin_lock_irq(&zone->lru_lock);

	if (global_reclaim(sc)) {
		if (current_is_kswapd())
			__count_zone_vm_events(PGSTEAL_KSWAPD, zone,
					       nr_reclaimed);
		else
			__count_zone_vm_events(PGSTEAL_DIRECT, zone,
					       nr_reclaimed);
	}

	putback_pages(lruvec, &page_list);

	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, -nr_taken);

	spin_unlock_irq(&zone->lru_lock);

	free_hot_cold_page_list(&page_list, 1);

	/*
	 * If reclaim is isolating dirty pages under writeback, it implies
	 * that the long-lived page allocation rate is exceeding the page
	 * laundering rate. Either the global limits are not being effective
	 * at throttling processes due to the page distribution throughout
	 * zones or there is heavy usage of a slow backing device. The
	 * only option is to throttle from reclaim context which is not ideal
	 * as there is no guarantee the dirtying process is throttled in the
	 * same way balance_dirty_pages() manages.
	 *
	 * This scales the number of dirty pages that must be under writeback
	 * before throttling depending on priority. It is a simple backoff
	 * function that has the most effect in the range DEF_PRIORITY to
	 * DEF_PRIORITY-2 which is the priority reclaim is considered to be
	 * in trouble and reclaim is considered to be in trouble.
	 *
	 * DEF_PRIORITY   100% isolated pages must be PageWriteback to throttle
	 * DEF_PRIORITY-1  50% must be PageWriteback
	 * DEF_PRIORITY-2  25% must be PageWriteback, kswapd in trouble
	 * ...
	 * DEF_PRIORITY-6 For SWAP_CLUSTER_MAX isolated pages, throttle if any
	 *                     isolated page is PageWriteback
	 */
	if (nr_writeback && nr_writeback >=
			(nr_taken >> (DEF_PRIORITY - sc->priority)))
		wait_iff_congested(zone, BLK_RW_ASYNC, HZ/10);

	return nr_reclaimed;
}


/* Policy functions */
static void init_lruvec_clock_pro(struct lruvec **lruvec, struct zone *zone)
{
	enum clock_pro_list l;
	*lruvec = (struct lruvec *)alloc_bootmem(sizeof(struct lruvec));
	if (!(*lruvec)) {
		printk("init_lruvec_clock_pro(): Couldn't allocate memory for lruvec.\n");
		return;
	}
	for (l = CLOCK_PRO_ANON_COLD; l < NR_CLOCK_PRO_LISTS; l++) {
		INIT_LIST_HEAD(&((*lruvec)->lists[l]));
	}
	(*lruvec)->nr_cold_target = 2 * zone->watermark[WMARK_HIGH];
	(*lruvec)->nr_nonresident_scale = 0;
}

static void get_scan_count_clock_pro(struct lruvec *lruvec, struct scan_control *sc,
				unsigned long *nr, bool force_scan)
{
	unsigned long scan;
	enum clock_pro_list l;

	for (l = CLOCK_PRO_ANON_COLD; l <= CLOCK_PRO_FILE_HOT; l++) {
		if (l == CLOCK_PRO_ANON_HOT || l == CLOCK_PRO_FILE_HOT)
			continue;

		scan = get_clockpro_resident_list(lruvec, l);
		if (sc->priority) {
			scan >>= sc->priority;
			if (!scan && force_scan)
				scan = SWAP_CLUSTER_MAX;
		}
		nr[l] = scan;
	}
}

static int shrink_lruvec_clock_pro(struct lruvec *lruvec, struct scan_control *sc,
				   unsigned long *nr)
{
	unsigned long nr_to_scan;
	enum clock_pro_list l;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;

	while (nr[CLOCK_PRO_ANON_COLD] || nr[CLOCK_PRO_FILE_COLD]) {
		for (l = 0; l <= CLOCK_PRO_FILE_HOT; l++) {
			if (l == CLOCK_PRO_ANON_HOT || l == CLOCK_PRO_FILE_HOT)
				continue;

			if (nr[l]) {
				nr_to_scan = min_t(unsigned long,
						   nr[l], SWAP_CLUSTER_MAX);
				nr[l] -= nr_to_scan;

				nr_reclaimed += shrink_cold_list(nr_to_scan,
								 lruvec, sc, l);
			}
		}
		/*
		 * On large memory systems, scan >> priority can become
		 * really large. This is fine for the starting priority;
		 * we want to put equal scanning pressure on each zone.
		 * However, if the VM has a harder time of freeing pages,
		 * with multiple processes reclaiming pages, the total
		 * freeing target can get unreasonably large.
		 */
		if (nr_reclaimed >= nr_to_reclaim &&
		    sc->priority < DEF_PRIORITY)
			break;
	}

	return nr_reclaimed;
}

static unsigned long estimate_pageable_memory(void)
{
#if 0
	static unsigned long next_check;
	static unsigned long total = 0;

	if (!total || time_after(jiffies, next_check)) {
		struct zone *z;
		total = 0;
		for_each_zone(z)
			total += z->nr_resident;
		next_check = jiffies + HZ/10;
	}

	// gave 0 first time, SIGFPE in kernel sucks
	// hence the !total
#else
	unsigned long total = 0;
	struct zone *z;
	for_each_zone(z)
		total += get_clockpro_resident(z->lruvecs[PAGE_RECLAIM_CLOCKPRO]);
#endif
	return total;
}

/*
 * Rotate the non-resident hand; scale the rotation speed so that when all
 * hot hands have made one full revolution the non-resident hand will have
 * too.
 * @dh: number of pages the hot hand has moved
 */
static void nonres_term(struct lruvec *lruvec, unsigned long dh)
{
	unsigned long long cycles;
	unsigned long nr_count = __nonres_count();

	/*
	 *         |n1| Rhot     |N| Rhot
	 * Nhot = ----------- ~ ----------
	 *           |r1|           |R|
	 *
	 * NOTE depends on |N|, hence use the nonresident_forget() hook.
	 */
	cycles = lruvec->nr_nonresident_scale + 1ULL * dh * nr_count;
	lruvec->nr_nonresident_scale =
		do_div(cycles, estimate_pageable_memory() + 1UL);
	nonres_rotate(cycles);
	cold_target_dec(lruvec, cycles);
}

/*
 * We can only receive hot lru (i.e. CLOCK_PRO_ANON_HOT, CLOCK_PRO_FILE_HOT).
 */
static void balance_hot(unsigned long nr_to_scan,
			struct lruvec *lruvec,
			struct scan_control *sc,
			enum clock_pro_list lru)
{
	LIST_HEAD(l_hold);
	LIST_HEAD(l_tmp);

	unsigned long nr_taken, nr_scanned, vm_flags;
	unsigned long dct = 0;
	int pgdeactivate = 0;
	int nr_pages;
	int file = is_file_lru(lru);
	isolate_mode_t isolate_mode = 0;
	struct page *page;
	struct zone *zone = lruvec_zone(lruvec);

	lru_add_drain();

	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	spin_lock_irq(&zone->lru_lock);
	select_list_hand(lruvec, &lruvec->lists[lru], get_cold_lru(lru));
	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, isolate_mode, lru);

	if (global_reclaim(sc))
		zone->pages_scanned += nr_scanned;

	/* The lru is hot => take from the resident. */
	__mod_zone_page_state(zone, NR_LRU_BASE + lru, -nr_taken);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, nr_taken);
	spin_unlock_irq(&zone->lru_lock);

	while (!list_empty(&l_hold)) {
		cond_resched();
		page = lru_to_page(&l_hold);
		list_del(&page->lru);

		if (unlikely(!page_evictable(page))) {
			putback_lru_page(page);
			continue;
		}

		if (unlikely(buffer_heads_over_limit)) {
			if (page_has_private(page) && trylock_page(page)) {
				if (page_has_private(page))
					try_to_release_page(page, 0);
				unlock_page(page);
			}
		}

		nr_pages = hpage_nr_pages(page);
		if(PageHot(page)) {
			VM_BUG_ON(PageTest(page));

			if (!page_referenced(page, 0, sc->target_mem_cgroup,
					     &vm_flags)) {
				SetPageTest(page);
				pgdeactivate += nr_pages;
			}
		} else {
			if (PageTest(page)) {
				ClearPageTest(page);
				dct += nr_pages;
			}
		}

		ClearPageActive(page);	/*!! we are de-activating */
		list_add(&page->lru, &l_tmp);
	}

	/*
	 * Move pages back to the lru list.
	 */
	spin_lock_irq(&zone->lru_lock);
	while (!list_empty(&l_tmp)) {
		int hand = get_cold_lru(lru);

		page = lru_to_page(&l_tmp);
		VM_BUG_ON(PageLRU(page));
		SetPageLRU(page);
		nr_pages = hpage_nr_pages(page);

		if (PageHot(page) && PageTest(page)) {
			ClearPageHot(page);
			ClearPageTest(page);
			hand = lru;
		}
		list_move(&page->lru, &lruvec->lists[hand]);
		update_cold_resident_stats(page, lruvec, get_cold_lru(lru), lru,
					   nr_pages);

		if (put_page_testzero(page)) {
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_list(page, lruvec, lru);
			ClearPageHot(page);
			ClearPageTest(page);

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&zone->lru_lock);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&zone->lru_lock);
			} else
				list_add(&page->lru, &l_hold);
		}
	}
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, -nr_taken);
	nonres_term(lruvec, nr_taken);
	cold_target_dec(lruvec, dct);
	spin_unlock_irq(&zone->lru_lock);

	__count_zone_vm_events(PGREFILL, zone, nr_scanned);
	__count_vm_events(PGDEACTIVATE, pgdeactivate);

	free_hot_cold_page_list(&l_hold, 1);
}

static void balance_lruvec_clock_pro(struct lruvec *lruvec, struct scan_control *sc)
{
	/*
	 * Limit the hot hand to half a revolution.
	 */
	if (get_clockpro_cold(lruvec) < lruvec->nr_cold_target) {
		int i = 0;
		int nr_file = 1 + (get_clockpro_resident_list(lruvec, CLOCK_PRO_FILE_HOT)
				   / 2 * SWAP_CLUSTER_MAX);
		int nr_anon = 1 + (get_clockpro_resident_list(lruvec, CLOCK_PRO_ANON_HOT)
				   / 2 * SWAP_CLUSTER_MAX);
		int max = nr_file;

		if (max < nr_anon)
			max = nr_anon;

		for (; get_clockpro_cold(lruvec) < lruvec->nr_cold_target &&
		     i < max; ++i) {
			if (i < nr_file)
				balance_hot(SWAP_CLUSTER_MAX, lruvec, sc,
					    CLOCK_PRO_FILE_HOT);
			if (i < nr_anon)
				balance_hot(SWAP_CLUSTER_MAX, lruvec, sc,
					    CLOCK_PRO_ANON_HOT);
		}
	}
}

static void page_accessed_clock_pro(struct page *page)
{
	/*if (!PageHot(page) && !PageUnevictable(page) &&
			PageReferenced(page) && PageLRU(page)) {
		activate_page(page);
		ClearPageReferenced(page);
	} else if (!PageReferenced(page)) {
		SetPageReferenced(page);
	}*/
	SetPageReferenced(page);
}

static void activate_page_clock_pro(struct page *page, struct lruvec *lruvec)
{
	if (!PageHot(page)) {
		int nr_pages = hpage_nr_pages(page);
		int lru = page_lru_base_type(page);

		SetPageHot(page);
		ClearPageTest(page);

		mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru,
				      -nr_pages);

		__count_vm_event(PGACTIVATE);
	 }
}

static void deactivate_page_clock_pro(struct page *page, struct lruvec *lruvec,
				 bool page_writeback_or_dirty)
{
	bool hot = PageHot(page);
	int lru = page_lru_base_type(page);

	ClearPageActive(page);
	ClearPageReferenced(page);
	ClearPageHot(page);
	ClearPageTest(page);

	if (hot) {
		int nr_pages = hpage_nr_pages(page);
		mem_cgroup_update_lru_size(lruvec, lru, nr_pages);
		__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru,
				      nr_pages);
	}

	if (page_writeback_or_dirty) {
		SetPageReclaim(page);
	} else {
		list_move_tail(&page->lru, &lruvec->lists[lru]);
	}
}

static void update_reclaim_statistics_clock_pro(struct lruvec *lruvec, int type,
						int rotated)
{
	/* We don't account for page statistics in Clock-PRO in this func. */
}

static void add_page_to_list_clock_pro(struct page *page, struct lruvec *lruvec,
				       int lru)
{
	add_page_to_list(page, lruvec, lru);
}

static void del_page_from_list_clock_pro(struct page *page, struct lruvec *lruvec,
				    int lru)
{
	del_page_from_list(page, lruvec, lru);
}

static void add_page_clock_pro(struct page *page, struct lruvec *lruvec, int lru)
{
	add_page_to_list(page, lruvec, lru);
}

static void release_page_clock_pro(struct page *page, struct lruvec *lruvec, int lru,
			      bool batch_release)
{
	del_page_from_list(page, lruvec, lru);
}

/*
 * Reclaim/compaction is used for high-order allocation requests.
 */
static bool should_continue_reclaim_clock_pro(struct lruvec *lruvec,
					 unsigned long nr_reclaimed,
					 unsigned long nr_scanned,
					 struct scan_control *sc)
{
	unsigned long cold_lru_pages;
	unsigned long pages_for_compaction = (2UL << sc->order);

	cold_lru_pages = get_clockpro_cold(lruvec);
	if (sc->nr_reclaimed < pages_for_compaction &&
			cold_lru_pages > pages_for_compaction)
		return true;

	return false;
}

unsigned long global_reclaimable_pages_clock_pro(void)
{
	int nr = global_page_state(NR_ACTIVE_FILE) +
		 global_page_state(NR_ACTIVE_ANON);

	return nr;
}

unsigned long zone_reclaimable_pages_clock_pro(struct zone *zone)
{
	int nr = zone_page_state(zone, NR_ACTIVE_FILE) +
		 zone_page_state(zone, NR_ACTIVE_ANON);

	return nr;
}

static bool too_many_isolated_comapction_clock_pro(struct zone *zone)
{
	unsigned long isolated, nr_resident;
	nr_resident = zone_page_state(zone, NR_ACTIVE_FILE) +
		      zone_page_state(zone, NR_ACTIVE_ANON);
	isolated = zone_page_state(zone, NR_ISOLATED_FILE) +
		   zone_page_state(zone, NR_ISOLATED_ANON);
	return isolated > nr_resident / 2;
}

static void reset_zone_vmstat_clock_pro(struct lruvec *lruvec, struct zone *zone,
					bool evictable)
{
	enum lru_list lru;
	int nr_pages, index;
	struct page *page;

	if (evictable) {
		for_each_evictable_lru(lru) {
			index = NR_LRU_BASE + lru;
			nr_pages = zone_page_state(zone, index);
			__mod_zone_page_state(zone, index, -nr_pages);
			mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
			
			list_for_each_entry(page, &(lruvec->lists[lru]), lru) {
			
				if(!PageUnevictable(page) && PageLRU(page)) {
					add_to_history(page,  zone->history, HISTORY_EVICTABLE);
				}
			}
		}
	} else {
		index = NR_LRU_BASE + LRU_UNEVICTABLE;
		nr_pages = zone_page_state(zone, index);
		__mod_zone_page_state(zone, index, -nr_pages);
		mem_cgroup_update_lru_size(lruvec, LRU_UNEVICTABLE, -nr_pages);
		
		list_for_each_entry(page, &(lruvec->lists[LRU_UNEVICTABLE]), lru) {
			if(PageUnevictable(page)) {
				add_to_history(page,  zone->history, HISTORY_UNEVICTABLE);
			}
		}
	}
}

static void rotate_inactive_page_clock_pro(struct page *page, struct lruvec *lruvec)
{
	if (!PageHot(page)) {
		int lru = page_lru_base_type(page);
		list_move_tail(&page->lru, &lruvec->lists[lru]);
	}
}

struct list_head *get_lruvec_list_clock_pro(struct lruvec *lruvec, int lru)
{
	return NULL;
}

/* For testing purposes */
struct list_head* get_test_list_clock_pro(struct lruvec *lruvec, int lru)
{
	return NULL;
}
static void print_lruvec_clock_pro(struct zone *zone) {
	printk("CLOCK_PRO: print helper function.\n");
}


static int activate_clock_pro(struct page *page)
{
	int hot, test;

	hot = PageHot(page);
	test = PageTest(page);

	if (hot) {
		VM_BUG_ON(test);
	} else {
		if (test) {
			SetPageHot(page);
			/*
			 * Leave PG_test set for new hot pages in order to
			 * recognise them in put_candidates() and do accounting.
			 */
			return 1;
		} else {
			SetPageTest(page);
		}
	}

	return 0;
}

static int page_check_references_clock_pro(struct page *page, struct scan_control *sc)
{
	int referenced_ptes, referenced_page;
	unsigned long vm_flags;

	referenced_ptes = page_referenced(page, 1, sc->target_mem_cgroup,
					  &vm_flags);
	referenced_page = TestClearPageReferenced(page);

	if (PageHot(page))
		return PAGEREF_KEEP;

	if (referenced_ptes || referenced_page) {
		return PAGEREF_ACTIVATE;
	}

	return PAGEREF_RECLAIM;
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
static void isolate_clock_pro(struct page *page, struct lruvec *lruvec)
{
	if (PageLRU(page)) {
		int lru = page_lru(page);
		get_page(page);
		ClearPageLRU(page);
		
		del_page_from_list(page, lruvec, lru);
	}
}

static void putback_page_clock_pro(struct page *page)
{
	int lru;
	int active = !!TestClearPageActive(page);
	int was_unevictable = PageUnevictable(page);
	
	printk(KERN_ERR "putback page lru\n");
	
	list_del(&page->lru);
	dec_zone_page_state(page, NR_ISOLATED_ANON +
			page_is_file_cache(page));

	VM_BUG_ON(PageLRU(page));

redo:
	ClearPageUnevictable(page);

	if (page_evictable(page)) {
		/*
		 * For evictable pages, we can use the cache.
		 * In event of a race, worst case is we end up with an
		 * unevictable page on [in]active list.
		 * We know how to handle that.
		 */
		lru = active + page_lru_base_type(page);
		lru_cache_add_lru(page, lru);
	} else {
		/*
		 * Put unevictable pages directly on zone's unevictable
		 * list.
		 */
		lru = LRU_UNEVICTABLE;
		add_page_to_unevictable_list(page);
		/*
		 * When racing with an mlock or AS_UNEVICTABLE clearing
		 * (page is unlocked) make sure that if the other thread
		 * does not observe our setting of PG_lru and fails
		 * isolation/check_move_unevictable_pages,
		 * we see PG_mlocked/AS_UNEVICTABLE cleared below and move
		 * the page back to the evictable list.
		 *
		 * The other side is TestClearPageMlocked() or shmem_lock().
		 */
		smp_mb();
	}

	/*
	 * page's status can change while we move it among lru. If an evictable
	 * page is on unevictable list, it never be freed. To avoid that,
	 * check after we added it to the list, again.
	 */
	if (lru == LRU_UNEVICTABLE && page_evictable(page)) {
		if (!isolate_lru_page(page)) {
			put_page(page);
			goto redo;
		}
		/* This means someone else dropped this page from LRU
		 * So, it will be freed or putback to LRU again. There is
		 * nothing to do here.
		 */
	}

	if (was_unevictable && lru != LRU_UNEVICTABLE)
		count_vm_event(UNEVICTABLE_PGRESCUED);
	else if (!was_unevictable && lru == LRU_UNEVICTABLE)
		count_vm_event(UNEVICTABLE_PGCULLED);

	put_page(page);		/* drop ref from isolate */
}


static void add_page_unevictable_clock_pro(struct page *page, struct lruvec *lruvec)
{
	add_page_to_list(page, lruvec, CLOCK_PRO_UNEVICTABLE);
}

const struct page_reclaim_policy clockpro_page_reclaim_policy =
{
	/* Initialize the structures. */
	.init_lruvec = init_lruvec_clock_pro,

	/* Decide which pages to reclaim and actually do the reclaiming. */
	.get_scan_count = get_scan_count_clock_pro,
	.shrink_lruvec = shrink_lruvec_clock_pro,
	.balance_lruvec = balance_lruvec_clock_pro,

	/* Helpers used when deciding which pages to reclaim and compact.*/
	.should_continue_reclaim = should_continue_reclaim_clock_pro,
	.zone_reclaimable_pages = zone_reclaimable_pages_clock_pro,
	.global_reclaimable_pages = global_reclaimable_pages_clock_pro,
	.too_many_isolated_compaction = too_many_isolated_comapction_clock_pro,

	/* Capture activity and statistics */
	.page_accessed = page_accessed_clock_pro,
	.activate_page = activate_page_clock_pro,
	.deactivate_page = deactivate_page_clock_pro,
	.update_reclaim_statistics = update_reclaim_statistics_clock_pro,
	/* New */
	.activate = activate_clock_pro,
	.page_check_references = page_check_references_clock_pro,

	/* Add/remove pages from the lists. */
	.add_page_to_list = add_page_to_list_clock_pro,
	.del_page_from_list = del_page_from_list_clock_pro,
	.add_page = add_page_clock_pro,
	.release_page = release_page_clock_pro,

	/* Helpers used for specific scenarios. */
	.rotate_inactive_page = rotate_inactive_page_clock_pro,
	.get_lruvec_list = get_lruvec_list_clock_pro,
	.reset_zone_vmstat = reset_zone_vmstat_clock_pro,

	/* For testing pursposes. */
	.get_list = get_test_list_clock_pro,
	.print_lruvec= print_lruvec_clock_pro,

	/* For non-resident algorithms. */
	.nonres_remember = nonres_remember_clock_pro,
	.nonres_forget = nonres_forget_clock_pro,

	/*isolate one page from the LRU lists*/
	.isolate = isolate_clock_pro,
	.putback_page = putback_page_clock_pro,
	.add_page_unevictable = add_page_unevictable_clock_pro,
//	.hint_use_once = hint_use_once_clock_pro,
};
EXPORT_SYMBOL(clockpro_page_reclaim_policy);

#endif /* CONFIG_RECLAIM_POLICY */
