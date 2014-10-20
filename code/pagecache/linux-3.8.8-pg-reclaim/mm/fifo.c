#ifdef CONFIG_RECLAIM_POLICY
/*
 * Implements the FIFO page replacement class - based on the First-In First-Out
 * page replacement algorithm.
 * The statistics for the number of pages on the two lists that FIFO uses are
 * kept into 
 * NR_LRU_BASE == NR_INACTIVE_ANON <- FIFO_EVICTABLE
 * NR_UNEVICTABLE <- FIFO_UNEVICTABLE
 * And the statistics for isolated pages are kept into NR_ISOLATED_ANON.
 * The size of the FIFO list is not limited, but it is still propotional to the
 * size of the memory, and kept in reasonable size as a consequence of the page
 * replacement algorithm trying to respect the memory watermarks.
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

//#define DEBUG


#define lru_to_page_fifo(_head) (list_entry((_head)->prev, struct page, lru))

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page_fifo(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

#define FIFO_BASE 0
enum fifo_list {
	FIFO_EVICTABLE = FIFO_BASE,
	FIFO_UNEVICTABLE,
	NR_FIFO_LISTS
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
	struct list_head lists[NR_FIFO_LISTS];
};

//returns LRU_UNEVICTABLE for unevictable pages, 0 otherwise
static int get_lru(int lru)
{
	if (lru == LRU_UNEVICTABLE)
		return LRU_UNEVICTABLE;
	return 0;
}

/*static int get_fifo(int lru)
{
	if (lru == LRU_UNEVICTABLE)
		return FIFO_UNEVICTABLE;
	return FIFO_EVICTABLE;
}*/

static struct list_head *get_fifo_list(struct lruvec *lruvec, int lru)
{
	if (lru == LRU_UNEVICTABLE)
		return &lruvec->lists[FIFO_UNEVICTABLE];
	return &lruvec->lists[FIFO_EVICTABLE];
}

static unsigned long get_fifo_size(struct lruvec *lruvec, enum lru_list lru)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, get_lru(lru));

	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + get_lru(lru));
}

static void add_page_to_list(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, get_lru(lru), nr_pages);
	list_add(&page->lru, get_fifo_list(lruvec, lru));
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + get_lru(lru),
				nr_pages);
}

static void del_page_from_list(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	int nr_pages = hpage_nr_pages(page);
	mem_cgroup_update_lru_size(lruvec, get_lru(lru), -nr_pages);
	list_del(&page->lru);
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + get_lru(lru),
				-nr_pages);
}

/*
 * A direct reclaimer may isolate SWAP_CLUSTER_MAX pages from the LRU list and
 * then get resheduled. When there are massive number of tasks doing page
 * allocation, such sleeping direct reclaimers may keep piling up on each CPU,
 * the LRU list will go small and be scanned faster than necessary, leading to
 * unnecessary swapping, thrashing and OOM.
 */
static int too_many_isolated(struct zone *zone, struct scan_control *sc)
{
	unsigned long all, isolated;

	if (current_is_kswapd())
		return 0;

	if (!global_reclaim(sc))
		return 0;

	all = zone_page_state(zone, NR_LRU_BASE);
	isolated = zone_page_state(zone, NR_ISOLATED_ANON);

	/*
	 * GFP_NOIO/GFP_NOFS callers are allowed to isolate more pages, so they
	 * won't get blocked by normal direct-reclaimers, forming a circular
	 * deadlock.
	 */
	if ((sc->gfp_mask & GFP_IOFS) == GFP_IOFS)
		all >>= 3;

	return isolated > all;
}

/* Policy Methods */
static void init_lruvec_fifo(struct lruvec **lruvec, struct zone *zone)
{
	*lruvec = (struct lruvec *)alloc_bootmem(sizeof(struct lruvec));
	if (!(*lruvec)) {
		printk("init_lruvec_fifo(): Couldn't allocate memory for lruvec.\n");
		return;
	}
	INIT_LIST_HEAD(&((*lruvec)->lists[FIFO_EVICTABLE]));
	INIT_LIST_HEAD(&((*lruvec)->lists[FIFO_UNEVICTABLE]));
	
	printk(KERN_ERR "FIFO: finish init lruvec\n");
}

static noinline_for_stack void
putback_inactive_pages(struct lruvec *lruvec, struct list_head *page_list)
{
	struct zone *zone = lruvec_zone(lruvec);
	LIST_HEAD(pages_to_free);

	/*
	 * Put back any unfreeable pages.
	 */
	while (!list_empty(page_list)) {
		struct page *page = lru_to_page_fifo(page_list);
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
		add_page_to_list(page, lruvec, lru);

		if (put_page_testzero(page)) {
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_list(page, lruvec, lru);


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
		isolate_mode_t mode)
{
	struct list_head *src = &lruvec->lists[FIFO_EVICTABLE];
	unsigned long nr_taken = 0;
	unsigned long scan;

	for (scan = 0; scan < nr_to_scan && !list_empty(src); scan++) {
		struct page *page;
		int nr_pages;

		page = lru_to_page_fifo(src);
		prefetchw_prev_lru_page(page, src, flags);

		VM_BUG_ON(!PageLRU(page));

		switch (__isolate_lru_page(page, mode)) {
		case 0:
			nr_pages = hpage_nr_pages(page);
			mem_cgroup_update_lru_size(lruvec, FIFO_EVICTABLE,
						   -nr_pages);
			ClearPageActive(page);
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
	return nr_taken;
}

/*
 * In FIFO I directly use "shrink_inactive_list" instead of calling
 * "shrink_inactive_list" in "shrink_list"
 * shrink_inactive_list() is a helper for shrink_zone().  It returns the number
 * of reclaimed pages
 */
static noinline_for_stack unsigned long
shrink_list(unsigned long nr_to_scan, struct lruvec *lruvec,
	    struct scan_control *sc)
{
	LIST_HEAD(page_list);
	unsigned long nr_scanned;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_taken;
	unsigned long nr_dirty = 0;
	unsigned long nr_writeback = 0;
	isolate_mode_t isolate_mode = 0;
	struct zone *zone = lruvec_zone(lruvec);

	while (unlikely(too_many_isolated(zone, sc))) {
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
				     &nr_scanned, sc, isolate_mode);

	__mod_zone_page_state(zone, NR_LRU_BASE, -nr_taken);
	/* Store the number of isolated pages in the NR_ISOLATED_ANON index. */
	__mod_zone_page_state(zone, NR_ISOLATED_ANON, nr_taken);

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

	putback_inactive_pages(lruvec, &page_list);

	__mod_zone_page_state(zone, NR_ISOLATED_ANON, -nr_taken);

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

static int shrink_lruvec_fifo(struct lruvec *lruvec, struct scan_control *sc,
			       unsigned long *nr)
{
	unsigned long nr_to_scan;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;

	/* Shrink only the evictable list. */
	while (nr[FIFO_EVICTABLE]) {
		nr_to_scan = min_t(unsigned long, nr[FIFO_EVICTABLE], SWAP_CLUSTER_MAX);
		nr[FIFO_EVICTABLE] -= nr_to_scan;
		nr_reclaimed += shrink_list(nr_to_scan, lruvec, sc);
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

static void add_page_to_list_fifo(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	add_page_to_list(page, lruvec, lru);
}

static void del_page_from_list_fifo(struct page *page, struct lruvec *lruvec,
				 int lru)
{
	del_page_from_list(page, lruvec, lru);
}

void add_page_unevictable_fifo(struct page *page, struct lruvec *lruvec)
{
	add_page_to_list(page, lruvec, FIFO_UNEVICTABLE);
}

static void add_page_fifo(struct page *page, struct lruvec *lruvec, int lru)
{
	add_page_to_list(page, lruvec, lru);
}

static void release_page_fifo(struct page *page, struct lruvec *lruvec, int lru,
			     bool batch_release)
{
	del_page_from_list(page, lruvec, lru);
}

/*
 * Reclaim/compaction is used for high-order allocation requests.
 */
static bool should_continue_reclaim_fifo(struct lruvec *lruvec,
					 unsigned long nr_reclaimed,
					 unsigned long nr_scanned,
					 struct scan_control *sc)
{
	unsigned long reclaimable_pages;
	unsigned long pages_for_compaction = (2UL << sc->order);

	printk("should_continue_reclaim_fifo():pages_for_compaction:%lu\n", pages_for_compaction);

	reclaimable_pages = get_fifo_size(lruvec, FIFO_EVICTABLE);
	if (sc->nr_reclaimed < pages_for_compaction && 
	    reclaimable_pages > pages_for_compaction)
		return true;

	return false;
}

unsigned long global_reclaimable_pages_fifo(void)
{
	return global_page_state(NR_LRU_BASE);
}

unsigned long zone_reclaimable_pages_fifo(struct zone *zone)
{
	return zone_page_state(zone, NR_LRU_BASE);
}

/*
 * Determine how aggressively the fifo list should be scanned. Since we keep
 * both anon and file pages on a single list we always scan depedning on
 * scan priority and at least SWAP_CLUSTER_MAX pages if the priority is low.
 */
static void get_scan_count_fifo(struct lruvec *lruvec, struct scan_control *sc,
				unsigned long *nr, bool force_scan)
{
	unsigned long scan;

	scan = get_fifo_size(lruvec, FIFO_EVICTABLE);
	if (sc->priority) {
		scan >>= sc->priority;
		if (!scan && force_scan)
			scan = SWAP_CLUSTER_MAX;
		/*
		 * Since anon and file pages are kept on a single list, double
		 * the number of pages that are to be scanned.
		 */
		//scan <<= 1;
	}
	nr[FIFO_EVICTABLE] = scan;
}

static bool too_many_isolated_comapction_fifo(struct zone *zone)
{
	unsigned long all, isolated;

	all = zone_page_state(zone, NR_LRU_BASE);
	isolated = zone_page_state(zone, NR_ISOLATED_ANON);

	return isolated > all;
}

static void rotate_inactive_page_fifo(struct page *page, struct lruvec *lruvec)
{
	list_move_tail(&page->lru, &lruvec->lists[FIFO_EVICTABLE]);
}

static void reset_zone_vmstat_fifo(struct lruvec *lruvec, struct zone *zone,
				   bool evictable)
{
	int index, nr_pages;
	int lru = evictable ? LRU_BASE : LRU_UNEVICTABLE;

	index = NR_LRU_BASE + lru;
	nr_pages = zone_page_state(zone, index);
	__mod_zone_page_state(zone, index, -nr_pages);
	mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
	
	nr_pages = zone_page_state(zone, NR_ISOLATED_ANON);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON, -nr_pages);
	nr_pages = zone_page_state(zone, NR_ISOLATED_FILE);
	__mod_zone_page_state(zone, NR_ISOLATED_FILE, -nr_pages);
}

static void balance_lruvec_fifo(struct lruvec *lruvec, struct scan_control *sc)
{
	/* We have a single list so no need to balance lists. */
}

static void page_accessed_fifo(struct page *page)
{
	/* A page has been accessed. We don't do anything in FIFO. */
}

static void activate_page_fifo(struct page *page, struct lruvec *lruvec)
{
	/* Don't do anything if the page is accessed a second time, which means
	 * it's activated. */
}

static void deactivate_page_fifo(struct page *page, struct lruvec *lruvec,
				 bool page_writeback_or_dirty)
{
	/* Don't do anything if the page has been inactive for a while.
	 * It's FIFO. */
}

static void update_reclaim_statistics_fifo(struct lruvec *lruvec, int type,
					   int rotated)
{
	/* We don't account for page statistics for now. */
}

/* For testing purposes */
struct list_head* get_test_list_fifo(struct lruvec *lruvec, int lru)
{
	return &lruvec->lists[lru];
}
static void print_lruvec_fifo(struct zone *zone) {
	printk("FIFO: print helper function.\n");
}

void nonres_remember_fifo(struct zone *zone, struct page *page)
{
	
}

static void nonres_forget_fifo(struct address_space *mapping, unsigned long index)
{
}

static int activate_fifo(struct page *page)
{
	VM_BUG_ON(PageActive(page));
	SetPageActive(page);
	return 1;
}

static int page_check_references_fifo(struct page *page, struct scan_control *sc)
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

const struct page_reclaim_policy fifo_page_reclaim_policy =
{
	/* Initialize the structures. */
	.init_lruvec = init_lruvec_fifo,

	/* Decide which pages to reclaim and actually do the reclaiming. */
	.get_scan_count = get_scan_count_fifo,
	.shrink_lruvec = shrink_lruvec_fifo,
	.balance_lruvec = balance_lruvec_fifo,

	/* Helpers used when deciding which pages to reclaim and compact.*/
	.should_continue_reclaim = should_continue_reclaim_fifo,
	.zone_reclaimable_pages = zone_reclaimable_pages_fifo,
	.global_reclaimable_pages = global_reclaimable_pages_fifo,
	.too_many_isolated_compaction = too_many_isolated_comapction_fifo,

	/* Capture activity and statistics */
	.page_accessed = page_accessed_fifo,
	.activate_page = activate_page_fifo,
	.deactivate_page = deactivate_page_fifo,
	.update_reclaim_statistics = update_reclaim_statistics_fifo,

	/* Add/remove pages from the lists. */
	.add_page_to_list = add_page_to_list_fifo,
	.del_page_from_list = del_page_from_list_fifo,
	.add_page = add_page_fifo,
	.release_page = release_page_fifo,

	/* Helpers used for specific scenarios. */
	.rotate_inactive_page = rotate_inactive_page_fifo,
	.get_lruvec_list = get_fifo_list,
	.reset_zone_vmstat = reset_zone_vmstat_fifo,

	/* For testing pursposes. */
	.get_list = get_test_list_fifo,
	.print_lruvec= print_lruvec_fifo,
	
	/*For nonresident lists*/
	.nonres_remember = nonres_remember_fifo,
	.nonres_forget = nonres_forget_fifo,
	.add_page_unevictable = add_page_unevictable_fifo,

	/* New */
	.activate = activate_fifo,
	.page_check_references = page_check_references_fifo,
};
EXPORT_SYMBOL(fifo_page_reclaim_policy);

#endif /* CONFIG_RECLAIM_POLICY */
