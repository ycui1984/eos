#ifdef CONFIG_RECLAIM_POLICY
/*
 * Implements the LRU page replacement class - based on the Least Recently Used
 * page replacement algorithm.
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

//#define DEBUG		1

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



struct zone_reclaim_stat {
	/*
	 * The pageout code in vmscan.c keeps track of how many of the
	 * mem/swap backed and file backed pages are referenced.
	 * The higher the rotated/scanned ratio, the more valuable
	 * that cache is.
	 *
	 * The anon LRU stats live in [0], file LRU stats in [1]
	 */
	unsigned long		recent_rotated[2];
	unsigned long		recent_scanned[2];
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
	struct list_head lists[NR_LRU_LISTS];
	struct zone_reclaim_stat reclaim_stat;
};

/* Interface functions for lruvec and zone_reclaim_stat. */
struct list_head *get_lruvec_list_lru(struct lruvec *lruvec, int lru)
{
	return &lruvec->lists[lru];
}

struct zone_reclaim_stat *get_reclaim_stat(struct lruvec *lruvec)
{
	return &lruvec->reclaim_stat;
}

unsigned long get_scanned(struct zone_reclaim_stat *reclaim_stat, int lru)
{
	return reclaim_stat->recent_scanned[lru];
}

unsigned long get_rotated(struct zone_reclaim_stat *reclaim_stat, int lru)
{
	return reclaim_stat->recent_rotated[lru];
}

void update_scanned(struct zone_reclaim_stat *reclaim_stat, int lru,
		    unsigned long n)
{
	reclaim_stat->recent_scanned[lru] += n;
}

void update_rotated(struct zone_reclaim_stat *reclaim_stat, int lru,
		    unsigned long n)
{
	reclaim_stat->recent_rotated[lru] += n;
}

/* from linux/mm_inline.h */
void add_page_to_lru_list(struct page *page, struct lruvec *lruvec, int lru)
{
//	struct zone *zone = lruvec_zone(lruvec);

	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, lru, nr_pages);
	list_add(&page->lru, get_lruvec_list_lru(lruvec, lru));
	
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru, nr_pages);
}

void del_page_from_lru_list(struct page *page, struct lruvec *lruvec, int lru)
{
	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
	list_del(&page->lru);
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru, -nr_pages);
}

/**
 * page_lru_base_type - which LRU list type should a page be on?
 * @page: the page to test
 *
 * Used for LRU list index arithmetic.
 *
 * Returns the base LRU type - file or anon - @page should be on.
 */
int page_lru_base_type(struct page *page)
{
	if (page_is_file_cache(page))
		return LRU_INACTIVE_FILE;
	return LRU_INACTIVE_ANON;
}

/**
 * page_off_lru - which LRU list was page on? clearing its lru flags.
 * @page: the page to test
 *
 * Returns the LRU list a page was on, as an index into the array of LRU
 * lists; and clears its Unevictable or Active flags, ready for freeing.
 */
int page_off_lru(struct page *page)
{
	enum lru_list lru;

	if (PageUnevictable(page)) {
		__ClearPageUnevictable(page);
		lru = LRU_UNEVICTABLE;
	} else {
		lru = page_lru_base_type(page);
		if (PageActive(page)) {
			__ClearPageActive(page);
			lru += LRU_ACTIVE;
		}
	}
	return lru;
}

/**
 * page_lru - which LRU list should a page be on?
 * @page: the page to test
 *
 * Returns the LRU list a page should be on, as an index
 * into the array of LRU lists.
 */
int page_lru(struct page *page)
{
	enum lru_list lru;

	if (PageUnevictable(page))
		lru = LRU_UNEVICTABLE;
	else {
		lru = page_lru_base_type(page);
		if (PageActive(page))
			lru += LRU_ACTIVE;
	}
	return lru;
}

/* START from vmscan.c */
static unsigned long get_lru_size(struct lruvec *lruvec, enum lru_list lru)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, lru);

	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru);
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
	unsigned long inactive, isolated;

	if (current_is_kswapd())
		return 0;

	if (!global_reclaim(sc))
		return 0;

	if (file) {
		inactive = zone_page_state(zone, NR_INACTIVE_FILE);
		isolated = zone_page_state(zone, NR_ISOLATED_FILE);
	} else {
		inactive = zone_page_state(zone, NR_INACTIVE_ANON);
		isolated = zone_page_state(zone, NR_ISOLATED_ANON);
	}

	/*
	 * GFP_NOIO/GFP_NOFS callers are allowed to isolate more pages, so they
	 * won't get blocked by normal direct-reclaimers, forming a circular
	 * deadlock.
	 */
	if ((sc->gfp_mask & GFP_IOFS) == GFP_IOFS)
		inactive >>= 3;

	return isolated > inactive;
}

static noinline_for_stack void
putback_inactive_pages(struct lruvec *lruvec, struct list_head *page_list)
{
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	struct zone *zone = lruvec_zone(lruvec);
	LIST_HEAD(pages_to_free);

	/*
	 * Put back any unfreeable pages.
	 */
	while (!list_empty(page_list)) {
		struct page *page = lru_to_page(page_list);
		int lru;

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
		add_page_to_lru_list(page, lruvec, lru);

		if (is_active_lru(lru)) {
			int file = is_file_lru(lru);
			int numpages = hpage_nr_pages(page);
			reclaim_stat->recent_rotated[file] += numpages;

		}
		if (put_page_testzero(page)) {
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_lru_list(page, lruvec, lru);
			
//			delete_from_history(page, zone->history,
//					    get_history_index(lru));
			
			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&zone->lru_lock);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&zone->lru_lock);
			} else
				list_add(&page->lru, &pages_to_free);
		}
	}

	/*
	 * To save our caller's stack, now use input list for pages to free.
	 */
	list_splice(&pages_to_free, page_list);
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
 * @lruvec:	The LRU vector to pull pages from.
 * @dst:	The temp list to put pages on to.
 * @nr_scanned:	The number of pages that were scanned.
 * @sc:		The scan_control struct for this reclaim session
 * @mode:	One of the LRU isolation modes
 * @lru:	LRU list id for isolating
 *
 * returns how many pages were moved onto *@dst.
 */
static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		isolate_mode_t mode, enum lru_list lru)
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
			mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
			list_move(&page->lru, dst);
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
	/*!!trace_mm_vmscan_lru_isolate(sc->order, nr_to_scan, scan,
				    nr_taken, mode, is_file_lru(lru));*/
	return nr_taken;
}

/*
 * shrink_inactive_list() is a helper for shrink_zone().  It returns the number
 * of reclaimed pages
 */
static noinline_for_stack unsigned long
shrink_inactive_list(unsigned long nr_to_scan, struct lruvec *lruvec,
		     struct scan_control *sc, enum lru_list lru)
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
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;

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

	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &page_list,
				     &nr_scanned, sc, isolate_mode, lru);

	__mod_zone_page_state(zone, NR_LRU_BASE + lru, -nr_taken);
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

	reclaim_stat->recent_scanned[file] += nr_taken;

	if (global_reclaim(sc)) {
		if (current_is_kswapd())
			__count_zone_vm_events(PGSTEAL_KSWAPD, zone,
					       nr_reclaimed);
		else
			__count_zone_vm_events(PGSTEAL_DIRECT, zone,
					       nr_reclaimed);
	}

	putback_inactive_pages(lruvec, &page_list);

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

	/*!!trace_mm_vmscan_lru_shrink_inactive(zone->zone_pgdat->node_id,
		zone_idx(zone),
		nr_scanned, nr_reclaimed,
		sc->priority,
		trace_shrink_flags(file));*/
	return nr_reclaimed;
}

/*
 * This moves pages from the active list to the inactive list.
 *
 * We move them the other way if the page is referenced by one or more
 * processes, from rmap.
 *
 * If the pages are mostly unmapped, the processing is fast and it is
 * appropriate to hold zone->lru_lock across the whole operation.  But if
 * the pages are mapped, the processing is slow (page_referenced()) so we
 * should drop zone->lru_lock around each page.  It's impossible to balance
 * this, so instead we remove the pages from the LRU while processing them.
 * It is safe to rely on PG_active against the non-LRU pages in here because
 * nobody will play with that bit on a non-LRU page.
 *
 * The downside is that we have to touch page->_count against each page.
 * But we had to alter page->flags anyway.
 */
static void move_active_pages_to_lru(struct lruvec *lruvec,
				     struct list_head *list,
				     struct list_head *pages_to_free,
				     enum lru_list lru)
{
	struct zone *zone = lruvec_zone(lruvec);
	unsigned long pgmoved = 0;
	struct page *page;
	int nr_pages;

	while (!list_empty(list)) {
		page = lru_to_page(list);
		lruvec = mem_cgroup_page_lruvec(page, zone);

		VM_BUG_ON(PageLRU(page));
		SetPageLRU(page);

		nr_pages = hpage_nr_pages(page);
		mem_cgroup_update_lru_size(lruvec, lru, nr_pages);
		list_move(&page->lru, &lruvec->lists[lru]);

		pgmoved += nr_pages;

		if (put_page_testzero(page)) {
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_lru_list(page, lruvec, lru);
			
//			delete_from_history(page, zone->history,
//					    get_history_index(lru));

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&zone->lru_lock);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&zone->lru_lock);
			} else
				list_add(&page->lru, pages_to_free);
		}
	}
	__mod_zone_page_state(zone, NR_LRU_BASE + lru, pgmoved);
	if (!is_active_lru(lru))
		__count_vm_events(PGDEACTIVATE, pgmoved);
}

static void shrink_active_list(unsigned long nr_to_scan,
			       struct lruvec *lruvec,
			       struct scan_control *sc,
			       enum lru_list lru)
{
	unsigned long nr_taken;
	unsigned long nr_scanned;
	unsigned long vm_flags;
	LIST_HEAD(l_hold);	/* The pages which were snipped off */
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);
	struct page *page;
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	unsigned long nr_rotated = 0;
	isolate_mode_t isolate_mode = 0;
	int file = is_file_lru(lru);
	struct zone *zone = lruvec_zone(lruvec);

	lru_add_drain();

	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	spin_lock_irq(&zone->lru_lock);

	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, isolate_mode, lru);
	if (global_reclaim(sc))
		zone->pages_scanned += nr_scanned;

	reclaim_stat->recent_scanned[file] += nr_taken;

	__count_zone_vm_events(PGREFILL, zone, nr_scanned);
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

		if (page_referenced(page, 0, sc->target_mem_cgroup,
				    &vm_flags)) {
			nr_rotated += hpage_nr_pages(page);
			/*
			 * Identify referenced, file-backed active pages and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon pages
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC pages,
			 * so we ignore them here.
			 */
			if ((vm_flags & VM_EXEC) && page_is_file_cache(page)) {
				list_add(&page->lru, &l_active);
				continue;
			}
		}

		ClearPageActive(page);	/* we are de-activating */
		list_add(&page->lru, &l_inactive);
	}

	/*
	 * Move pages back to the lru list.
	 */
	spin_lock_irq(&zone->lru_lock);
	/*
	 * Count referenced pages from currently used mappings as rotated,
	 * even though only some of them are actually re-activated.  This
	 * helps balance scan pressure between file and anonymous pages in
	 * get_scan_ratio.
	 */
	reclaim_stat->recent_rotated[file] += nr_rotated;

	move_active_pages_to_lru(lruvec, &l_active, &l_hold, lru);
	move_active_pages_to_lru(lruvec, &l_inactive, &l_hold, lru - LRU_ACTIVE);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, -nr_taken);
	spin_unlock_irq(&zone->lru_lock);

	free_hot_cold_page_list(&l_hold, 1);
}

#ifdef CONFIG_SWAP
static int inactive_anon_is_low_global(struct zone *zone)
{
	unsigned long active, inactive;

	active = zone_page_state(zone, NR_ACTIVE_ANON);
	inactive = zone_page_state(zone, NR_INACTIVE_ANON);

	if (inactive * zone->inactive_ratio < active)
		return 1;

	return 0;
}

/**
 * inactive_anon_is_low - check if anonymous pages need to be deactivated
 * @lruvec: LRU vector to check
 *
 * Returns true if the zone does not have enough inactive anon pages,
 * meaning some active anon pages need to be deactivated.
 */
static int inactive_anon_is_low(struct lruvec *lruvec)
{
	/*
	 * If we don't have swap space, anonymous page deactivation
	 * is pointless.
	 */
	if (!total_swap_pages)
		return 0;

	if (!mem_cgroup_disabled())
		return mem_cgroup_inactive_anon_is_low(lruvec);

	return inactive_anon_is_low_global(lruvec_zone(lruvec));
}
#else /* CONFIG_SWAP */
static inline int inactive_anon_is_low(struct lruvec *lruvec)
{
	return 0;
}
#endif /* CONFIG_SWAP */

static int inactive_file_is_low_global(struct zone *zone)
{
	unsigned long active, inactive;

	active = zone_page_state(zone, NR_ACTIVE_FILE);
	inactive = zone_page_state(zone, NR_INACTIVE_FILE);

	return (active > inactive);
}

/**
 * inactive_file_is_low - check if file pages need to be deactivated
 * @lruvec: LRU vector to check
 *
 * When the system is doing streaming IO, memory pressure here
 * ensures that active file pages get deactivated, until more
 * than half of the file pages are on the inactive list.
 *
 * Once we get to that situation, protect the system's working
 * set from being evicted by disabling active file page aging.
 *
 * This uses a different ratio than the anonymous pages, because
 * the page cache uses a use-once replacement algorithm.
 */
static int inactive_file_is_low(struct lruvec *lruvec)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_inactive_file_is_low(lruvec);

	return inactive_file_is_low_global(lruvec_zone(lruvec));
}

static int inactive_list_is_low(struct lruvec *lruvec, enum lru_list lru)
{
	if (is_file_lru(lru))
		return inactive_file_is_low(lruvec);
	else
		return inactive_anon_is_low(lruvec);
}

static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan,
				 struct lruvec *lruvec, struct scan_control *sc)
{
	if (is_active_lru(lru)) {
		if (inactive_list_is_low(lruvec, lru))
			shrink_active_list(nr_to_scan, lruvec, sc, lru);
		return 0;
	}

	return shrink_inactive_list(nr_to_scan, lruvec, sc, lru);
}

static int vmscan_swappiness(struct scan_control *sc)
{
	if (global_reclaim(sc))
		return vm_swappiness;
	return mem_cgroup_swappiness(sc->target_mem_cgroup);
}
/* END from vmscan.c */

/* Policy functions. */
static void init_lruvec_lru(struct lruvec **lruvec, struct zone *zone)
{
	enum lru_list lru;

	*lruvec = (struct lruvec *)alloc_bootmem(sizeof(struct lruvec));
	if (!(*lruvec)) {
		printk("init_lruvec_lru(): Couldn't allocate memory for lruvec.\n");
		return;
	}
	memset(*lruvec, 0, sizeof(struct lruvec));

	for_each_lru(lru)
		INIT_LIST_HEAD(&((*lruvec)->lists[lru]));
}

/*
 * If we have not reclaimed enough pages for compaction and the
 * inactive lists are large enough, continue reclaiming
 */
static bool should_continue_reclaim_lru(struct lruvec *lruvec,
					unsigned long nr_reclaimed,
					unsigned long nr_scanned,
					struct scan_control *sc)
{
	unsigned long inactive_lru_pages;
	unsigned long pages_for_compaction = (2UL << sc->order);

	inactive_lru_pages = get_lru_size(lruvec, LRU_INACTIVE_FILE);
	if (nr_swap_pages > 0)
		inactive_lru_pages += get_lru_size(lruvec, LRU_INACTIVE_ANON);
	if (sc->nr_reclaimed < pages_for_compaction &&
			inactive_lru_pages > pages_for_compaction)
		return true;

	return false;
}

static int shrink_lruvec_lru(struct lruvec *lruvec, struct scan_control *sc,
			     unsigned long *nr)
{
	unsigned long nr_to_scan;
	enum lru_list lru;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;

	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] ||
					nr[LRU_INACTIVE_FILE]) {
		for_each_evictable_lru(lru) {
			if (nr[lru]) {
				nr_to_scan = min_t(unsigned long,
						   nr[lru], SWAP_CLUSTER_MAX);
				nr[lru] -= nr_to_scan;

				nr_reclaimed += shrink_list(lru, nr_to_scan,
							    lruvec, sc);
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

static void balance_lruvec_lru(struct lruvec *lruvec, struct scan_control *sc)
{
	/*
	 * Even if we did not try to evict anon pages at all, we want to
	 * rebalance the anon lru active/inactive ratio.
	 */
	if (inactive_anon_is_low(lruvec))
		shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
				   sc, LRU_ACTIVE_ANON);
}

/*
 * Determine how aggressively the anon and file LRU lists should be
 * scanned.  The relative value of each set of LRU lists is determined
 * by looking at the fraction of the pages scanned we did rotate back
 * onto the active list instead of evict.
 *
 * nr[0] = anon inactive pages to scan; nr[1] = anon active pages to scan
 * nr[2] = file inactive pages to scan; nr[3] = file active pages to scan
 */
static void get_scan_count_lru(struct lruvec *lruvec, struct scan_control *sc,
			       unsigned long *nr, bool force_scan)
{
	unsigned long anon, file, free;
	unsigned long anon_prio, file_prio;
	unsigned long ap, fp;
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	u64 fraction[2], denominator;
	enum lru_list lru;
	int noswap = 0;
	struct zone *zone = lruvec_zone(lruvec);

	/* If we have no swap space, do not bother scanning anon pages. */
	if (!sc->may_swap || (nr_swap_pages <= 0)) {
		noswap = 1;
		fraction[0] = 0;
		fraction[1] = 1;
		denominator = 1;
		goto out;
	}

	anon  = get_lru_size(lruvec, LRU_ACTIVE_ANON) +
		get_lru_size(lruvec, LRU_INACTIVE_ANON);
	file  = get_lru_size(lruvec, LRU_ACTIVE_FILE) +
		get_lru_size(lruvec, LRU_INACTIVE_FILE);

	if (global_reclaim(sc)) {
		free  = zone_page_state(zone, NR_FREE_PAGES);
		if (unlikely(file + free <= high_wmark_pages(zone))) {
			/*
			 * If we have very few page cache pages, force-scan
			 * anon pages.
			 */
			fraction[0] = 1;
			fraction[1] = 0;
			denominator = 1;
			goto out;
		} else if (!inactive_file_is_low_global(zone)) {
			/*
			 * There is enough inactive page cache, do not
			 * reclaim anything from the working set right now.
			 */
			fraction[0] = 0;
			fraction[1] = 1;
			denominator = 1;
			goto out;
		}
	}

	/*
	 * With swappiness at 100, anonymous and file have the same priority.
	 * This scanning priority is essentially the inverse of IO cost.
	 */
	anon_prio = vmscan_swappiness(sc);
	file_prio = 200 - anon_prio;

	/*
	 * OK, so we have swap space and a fair amount of page cache
	 * pages.  We use the recently rotated / recently scanned
	 * ratios to determine how valuable each cache is.
	 *
	 * Because workloads change over time (and to avoid overflow)
	 * we keep these statistics as a floating average, which ends
	 * up weighing recent references more than old ones.
	 *
	 * anon in [0], file in [1]
	 */
	spin_lock_irq(&zone->lru_lock);
	if (unlikely(reclaim_stat->recent_scanned[0] > anon / 4)) {
		reclaim_stat->recent_scanned[0] /= 2;
		reclaim_stat->recent_rotated[0] /= 2;
	}

	if (unlikely(reclaim_stat->recent_scanned[1] > file / 4)) {
		reclaim_stat->recent_scanned[1] /= 2;
		reclaim_stat->recent_rotated[1] /= 2;
	}

	/*
	 * The amount of pressure on anon vs file pages is inversely
	 * proportional to the fraction of recently scanned pages on
	 * each list that were recently referenced and in active use.
	 */
	ap = anon_prio * (reclaim_stat->recent_scanned[0] + 1);
	ap /= reclaim_stat->recent_rotated[0] + 1;

	fp = file_prio * (reclaim_stat->recent_scanned[1] + 1);
	fp /= reclaim_stat->recent_rotated[1] + 1;
	spin_unlock_irq(&zone->lru_lock);

	fraction[0] = ap;
	fraction[1] = fp;
	denominator = ap + fp + 1;
out:
	for_each_evictable_lru(lru) {
		int file = is_file_lru(lru);
		unsigned long scan;

		scan = get_lru_size(lruvec, lru);
		if (sc->priority || noswap || !vmscan_swappiness(sc)) {
			scan >>= sc->priority;
			if (!scan && force_scan)
				scan = SWAP_CLUSTER_MAX;
			scan = div64_u64(scan * fraction[file], denominator);
		}
		nr[lru] = scan;
	}
}

static void add_page_to_list_lru(struct page *page, struct lruvec *lruvec,
				 int list_index)
{
	add_page_to_lru_list(page, lruvec, list_index);
}

static void del_page_from_list_lru(struct page *page, struct lruvec *lruvec,
				   int list_index)
{
	del_page_from_lru_list(page, lruvec, list_index);
}

static bool too_many_isolated_comapction_lru(struct zone *zone)
{
	unsigned long active, inactive, isolated;

	inactive = zone_page_state(zone, NR_INACTIVE_FILE) +
					zone_page_state(zone, NR_INACTIVE_ANON);
	active = zone_page_state(zone, NR_ACTIVE_FILE) +
					zone_page_state(zone, NR_ACTIVE_ANON);
	isolated = zone_page_state(zone, NR_ISOLATED_FILE) +
					zone_page_state(zone, NR_ISOLATED_ANON);

	return isolated > (inactive + active) / 2;
}

/*
 * The reclaimable count would be mostly accurate.
 * The less reclaimable pages may be
 * - mlocked pages, which will be moved to unevictable list when encountered
 * - mapped pages, which may require several travels to be reclaimed
 * - dirty pages, which is not "instantly" reclaimable
 */
static unsigned long global_reclaimable_pages_lru(void)
{
	int nr;

	nr = global_page_state(NR_ACTIVE_FILE) +
	     global_page_state(NR_INACTIVE_FILE);

	if (nr_swap_pages > 0)
		nr += global_page_state(NR_ACTIVE_ANON) +
		      global_page_state(NR_INACTIVE_ANON);

	return nr;
}

static unsigned long zone_reclaimable_pages_lru(struct zone *zone)
{
	int nr;

	nr = zone_page_state(zone, NR_ACTIVE_FILE) +
	     zone_page_state(zone, NR_INACTIVE_FILE);

	if (nr_swap_pages > 0)
		nr += zone_page_state(zone, NR_ACTIVE_ANON) +
		      zone_page_state(zone, NR_INACTIVE_ANON);

	return nr;
}

static void update_reclaim_statistics_lru(struct lruvec *lruvec, int type,
					  int rotated)
{
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;

	reclaim_stat->recent_scanned[type]++;
	if (rotated)
		reclaim_stat->recent_rotated[type]++;
}

static void page_accessed_lru(struct page *page)
{
	if (!PageActive(page) && !PageUnevictable(page) &&
			PageReferenced(page) && PageLRU(page)) {
		activate_page(page);
		ClearPageReferenced(page);
	} else if (!PageReferenced(page)) {
		SetPageReferenced(page);
	}
}

static void activate_page_lru(struct page *page, struct lruvec *lruvec)
{
	if (!PageActive(page)) {
		int file = page_is_file_cache(page);
		int lru = page_lru_base_type(page);

		del_page_from_lru_list(page, lruvec, lru);
		SetPageActive(page);
		lru += LRU_ACTIVE;
		add_page_to_lru_list(page, lruvec, lru);

		__count_vm_event(PGACTIVATE);
		update_reclaim_statistics_lru(lruvec, file, 1);
	}
}

static void deactivate_page_lru(struct page *page, struct lruvec *lruvec,
				bool page_writeback_or_dirty)
{
	bool active = PageActive(page);
	int lru = page_lru_base_type(page);

	del_page_from_lru_list(page, lruvec, lru + active);
	ClearPageActive(page);
	ClearPageReferenced(page);
	add_page_to_lru_list(page, lruvec, lru);

	if (page_writeback_or_dirty) {
		/*
		 * PG_reclaim could be raced with end_page_writeback
		 * It can make readahead confusing.  But race window
		 * is _really_ small and  it's non-critical problem.
		 */
		SetPageReclaim(page);
	} else {
		/*
		 * The page's writeback ends up during pagevec
		 * We moves tha page into tail of inactive.
		 */
		list_move_tail(&page->lru, &lruvec->lists[lru]);
	}
}

static void rotate_inactive_page_lru(struct page *page, struct lruvec *lruvec)
{
	if (!PageActive(page)) {
		enum lru_list lru = page_lru_base_type(page);
		list_move_tail(&page->lru, &lruvec->lists[lru]);
	}
}

static void add_page_lru(struct page *page, struct lruvec *lruvec, int lru)
{
	SetPageLRU(page);
	add_page_to_list_lru(page, lruvec, lru);
}

void add_page_unevictable_lru(struct page *page, struct lruvec *lruvec)
{
	add_page_to_list_lru(page, lruvec, LRU_UNEVICTABLE);
}

static void release_page_lru(struct page *page, struct lruvec *lruvec, int lru,
			     bool batch_release)
{
	del_page_from_list_lru(page, lruvec, lru);
}

static void reset_zone_vmstat_lru(struct lruvec *lruvec, struct zone *zone,
				  bool evictable)
{
	enum lru_list lru_evic;
	int nr_pages, index, nr_evictable = 0;
	struct page *page;

	if (evictable) {
		for_each_evictable_lru(lru_evic) {
			index = NR_LRU_BASE + lru_evic;

			nr_pages = zone_page_state(zone, index);
			__mod_zone_page_state(zone, index, -nr_pages);
			mem_cgroup_update_lru_size(lruvec, lru_evic, -nr_pages);
			
			list_for_each_entry(page, &(lruvec->lists[lru_evic]), lru) {
			
				if(!PageUnevictable(page) && PageLRU(page)) {
//					list_del(&page->lru);
					add_to_history(page,  zone->history, HISTORY_EVICTABLE);
				}
				nr_evictable ++;
			}

		}
	
		nr_pages = zone_page_state(zone, NR_ISOLATED_ANON);
		__mod_zone_page_state(zone, NR_ISOLATED_ANON, -nr_pages);
		nr_pages = zone_page_state(zone, NR_ISOLATED_FILE);
		__mod_zone_page_state(zone, NR_ISOLATED_FILE, -nr_pages);

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

struct list_head* get_test_list_lru(struct lruvec *lruvec, int lru)
{
	return &lruvec->lists[lru];
}

static void print_lruvec_lru(struct zone *zone)
{
	printk("LRU: Helper Function.\n");
}

void nonres_remember_lru(struct zone *zone, struct page *page)
{
	
}

void isolate_lru(struct page *page, struct lruvec *lruvec) 
{
//	printk(KERN_ERR "isolate lru\n");
	if (PageLRU(page)) {
		int lru = page_lru(page);
		get_page(page);
		ClearPageLRU(page);
		
		del_page_from_list_lru(page, lruvec, lru);
		}
}

void putback_page_lru(struct page *page)
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

static void nonres_forget_lru(struct address_space *mapping, unsigned long index)
{
}

static int activate_lru(struct page *page)
{
	VM_BUG_ON(PageActive(page));
	SetPageActive(page);
	return 1;
}

static int page_check_references_lru(struct page *page, struct scan_control *sc)
{
	int referenced_ptes, referenced_page;
	unsigned long vm_flags;

	referenced_ptes = page_referenced(page, 1, sc->target_mem_cgroup,
					  &vm_flags);
	referenced_page = TestClearPageReferenced(page);

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
}

const struct page_reclaim_policy lru_page_reclaim_policy =
{
	/* Initialize the structures. */
	.init_lruvec = init_lruvec_lru,

	/* Decide which pages to reclaim and actually do the reclaiming. */
	.get_scan_count = get_scan_count_lru,
	.shrink_lruvec = shrink_lruvec_lru,
	.balance_lruvec = balance_lruvec_lru,

	/* Helpers used when deciding which pages to reclaim and compact.*/
	.should_continue_reclaim = should_continue_reclaim_lru,
	.zone_reclaimable_pages = zone_reclaimable_pages_lru,
	.global_reclaimable_pages = global_reclaimable_pages_lru,
	.too_many_isolated_compaction = too_many_isolated_comapction_lru,

	/* Capture activity and statistics */
	.page_accessed = page_accessed_lru,
	.activate_page = activate_page_lru,
	.deactivate_page = deactivate_page_lru,
	.update_reclaim_statistics = update_reclaim_statistics_lru,

	/* Add/remove pages from the lists. */
	.add_page_to_list = add_page_to_list_lru,
	.del_page_from_list = del_page_from_list_lru,
	.add_page = add_page_lru,
	.release_page = release_page_lru,
	.add_page_unevictable = add_page_unevictable_lru,

	/* Helpers used for specific scenarios. */
	.rotate_inactive_page = rotate_inactive_page_lru,
	.get_lruvec_list = get_lruvec_list_lru,
	.reset_zone_vmstat = reset_zone_vmstat_lru,

	/* For testing purposes */
	.get_list = get_test_list_lru,
	.print_lruvec = print_lruvec_lru,
	
	/*For nonresident lists*/
	.nonres_remember = nonres_remember_lru,
	.nonres_forget = nonres_forget_lru,

	/*Isolate a page from its LRU list*/
	.isolate = isolate_lru,
	.putback_page = putback_page_lru,

	/* New */
	.activate = activate_lru,
	.page_check_references = page_check_references_lru,
//	.hint_use_once = hint_use_once_lru,
};
EXPORT_SYMBOL(lru_page_reclaim_policy);

#endif /* CONFIG_RECLAIM_POLICY */
