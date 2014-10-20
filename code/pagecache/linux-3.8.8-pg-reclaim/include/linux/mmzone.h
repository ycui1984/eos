#ifndef _LINUX_MMZONE_H
#define _LINUX_MMZONE_H

#ifndef __ASSEMBLY__
#ifndef __GENERATING_BOUNDS_H

#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/threads.h>
#include <linux/numa.h>
#include <linux/init.h>
#include <linux/seqlock.h>
#include <linux/nodemask.h>
#include <linux/pageblock-flags.h>
#include <generated/bounds.h>
#include <linux/atomic.h>
#include <asm/page.h>

#include <linux/fs.h>

/* Free memory management - zoned buddy allocator.  */
#ifndef CONFIG_FORCE_MAX_ZONEORDER
#define MAX_ORDER 11
#else
#define MAX_ORDER CONFIG_FORCE_MAX_ZONEORDER
#endif
#define MAX_ORDER_NR_PAGES (1 << (MAX_ORDER - 1))

/*
 * PAGE_ALLOC_COSTLY_ORDER is the order at which allocations are deemed
 * costly to service.  That is between allocation orders which should
 * coalesce naturally under reasonable reclaim pressure and those which
 * will not.
 */
#define PAGE_ALLOC_COSTLY_ORDER 3

enum {
	MIGRATE_UNMOVABLE,
	MIGRATE_RECLAIMABLE,
	MIGRATE_MOVABLE,
	MIGRATE_PCPTYPES,	/* the number of types on the pcp lists */
	MIGRATE_RESERVE = MIGRATE_PCPTYPES,
#ifdef CONFIG_CMA
	/*
	 * MIGRATE_CMA migration type is designed to mimic the way
	 * ZONE_MOVABLE works.  Only movable pages can be allocated
	 * from MIGRATE_CMA pageblocks and page allocator never
	 * implicitly change migration type of MIGRATE_CMA pageblock.
	 *
	 * The way to use it is to change migratetype of a range of
	 * pageblocks to MIGRATE_CMA which can be done by
	 * __free_pageblock_cma() function.  What is important though
	 * is that a range of pageblocks must be aligned to
	 * MAX_ORDER_NR_PAGES should biggest page be bigger then
	 * a single pageblock.
	 */
	MIGRATE_CMA,
#endif
	MIGRATE_ISOLATE,	/* can't allocate from here */
	MIGRATE_TYPES
};

#ifdef CONFIG_CMA
#  define is_migrate_cma(migratetype) unlikely((migratetype) == MIGRATE_CMA)
#else
#  define is_migrate_cma(migratetype) false
#endif

#define for_each_migratetype_order(order, type) \
	for (order = 0; order < MAX_ORDER; order++) \
		for (type = 0; type < MIGRATE_TYPES; type++)

extern int page_group_by_mobility_disabled;

static inline int get_pageblock_migratetype(struct page *page)
{
	return get_pageblock_flags_group(page, PB_migrate, PB_migrate_end);
}

struct free_area {
	struct list_head	free_list[MIGRATE_TYPES];
	unsigned long		nr_free;
};

struct pglist_data;

/*
 * zone->lock and zone->lru_lock are two of the hottest locks in the kernel.
 * So add a wild amount of padding here to ensure that they fall into separate
 * cachelines.  There are very few zone structures in the machine, so space
 * consumption is not a concern here.
 */
#if defined(CONFIG_SMP)
struct zone_padding {
	char x[0];
} ____cacheline_internodealigned_in_smp;
#define ZONE_PADDING(name)	struct zone_padding name;
#else
#define ZONE_PADDING(name)
#endif

enum zone_stat_item {
	/* First 128 byte cacheline (assuming 64 bit words) */
	NR_FREE_PAGES,
	NR_LRU_BASE,
	NR_INACTIVE_ANON = NR_LRU_BASE, /* must match order of LRU_[IN]ACTIVE */
	NR_ACTIVE_ANON,		/*  "     "     "   "       "         */
	NR_INACTIVE_FILE,	/*  "     "     "   "       "         */
	NR_ACTIVE_FILE,		/*  "     "     "   "       "         */
	NR_UNEVICTABLE,		/*  "     "     "   "       "         */
	NR_MLOCK,		/* mlock()ed pages found and moved off LRU */
	NR_ANON_PAGES,	/* Mapped anonymous pages */
	NR_FILE_MAPPED,	/* pagecache pages mapped into pagetables.
			   only modified from process context */
	NR_FILE_PAGES,
	NR_FILE_DIRTY,
	NR_WRITEBACK,
	NR_SLAB_RECLAIMABLE,
	NR_SLAB_UNRECLAIMABLE,
	NR_PAGETABLE,		/* used for pagetables */
	NR_KERNEL_STACK,
	/* Second 128 byte cacheline */
	NR_UNSTABLE_NFS,	/* NFS unstable pages */
	NR_BOUNCE,
	NR_VMSCAN_WRITE,
	NR_VMSCAN_IMMEDIATE,	/* Prioritise for reclaim when writeback ends */
	NR_WRITEBACK_TEMP,	/* Writeback using temporary buffers */
	NR_ISOLATED_ANON,	/* Temporary isolated pages from anon lru */
	NR_ISOLATED_FILE,	/* Temporary isolated pages from file lru */
	NR_SHMEM,		/* shmem pages (included tmpfs/GEM pages) */
	NR_DIRTIED,		/* page dirtyings since bootup */
	NR_WRITTEN,		/* page writings since bootup */
#ifdef CONFIG_NUMA
	NUMA_HIT,		/* allocated in intended node */
	NUMA_MISS,		/* allocated in non intended node */
	NUMA_FOREIGN,		/* was intended here, hit elsewhere */
	NUMA_INTERLEAVE_HIT,	/* interleaver preferred this zone */
	NUMA_LOCAL,		/* allocation from local node */
	NUMA_OTHER,		/* allocation from other node */
#endif
	NR_ANON_TRANSPARENT_HUGEPAGES,
	NR_FREE_CMA_PAGES,
#ifdef CONFIG_RECLAIM_POLICY
	NR_COLD_TARGET,
#endif
	NR_VM_ZONE_STAT_ITEMS };

/*
 * We do arithmetic on the LRU lists in various places in the code,
 * so it is important to keep the active lists LRU_ACTIVE higher in
 * the array than the corresponding inactive lists, and to keep
 * the *_FILE lists LRU_FILE higher than the corresponding _ANON lists.
 *
 * This has to be kept in sync with the statistics in zone_stat_item
 * above and the descriptions in vmstat_text in mm/vmstat.c
 */
#define LRU_BASE 0
#define LRU_ACTIVE 1
#define LRU_FILE 2

enum lru_list {
	LRU_INACTIVE_ANON = LRU_BASE,
	LRU_ACTIVE_ANON = LRU_BASE + LRU_ACTIVE,
	LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,
	LRU_ACTIVE_FILE = LRU_BASE + LRU_FILE + LRU_ACTIVE,
	LRU_UNEVICTABLE,
	NR_LRU_LISTS
};

#define for_each_lru(lru) for (lru = 0; lru < NR_LRU_LISTS; lru++)

#define for_each_evictable_lru(lru) for (lru = 0; lru <= LRU_ACTIVE_FILE; lru++)

static inline int is_file_lru(enum lru_list lru)
{
	return (lru == LRU_INACTIVE_FILE || lru == LRU_ACTIVE_FILE);
}

static inline int is_active_lru(enum lru_list lru)
{
	return (lru == LRU_ACTIVE_ANON || lru == LRU_ACTIVE_FILE);
}

static inline int is_unevictable_lru(enum lru_list lru)
{
	return (lru == LRU_UNEVICTABLE);
}

#ifdef CONFIG_RECLAIM_POLICY

#define PGRECLAIM_BASE		0
#define HISTORY_BASE		0

struct zone_reclaim_stat;
/* The base lruvec is defined in mm/mmzone.c. */
struct lruvec;
/*
 * Defined in mm/memcontrol.c
 */
struct mem_cgroup_per_zone;

enum pgreclaim_policy_list {
	/* The defualt page reclaim policy resembles simplified LRU-2Q,
	 * according to the following document, so I am naming the constant
	 * LRU2Q. https://www.kernel.org/doc/gorman/html/understand/understand013.html */
	PAGE_RECLAIM_LRU2Q = PGRECLAIM_BASE,
	PAGE_RECLAIM_FIFO,
	PAGE_RECLAIM_CLOCKPRO,
	PAGE_RECLAIM_ARC,
	PAGE_RECLAIM_CART,
	NR_PAGE_RECLAIM_POLICIES
};
extern enum pgreclaim_policy_list global_reclaim_policy;

extern const
struct page_reclaim_policy *pgreclaim_policies[NR_PAGE_RECLAIM_POLICIES];
/* Get the respective page_reclaim_policy. */
static inline const
struct page_reclaim_policy *reclaim_policy(enum pgreclaim_policy_list p) {
	return pgreclaim_policies[p];
}

enum history_list {
	HISTORY_EVICTABLE = HISTORY_BASE,
	HISTORY_UNEVICTABLE,
	NR_HISTORY_LISTS
};

#else /* CONFIG_RECLAIM_POLICY */
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
	struct list_head lists[NR_LRU_LISTS];
	struct zone_reclaim_stat reclaim_stat;
#ifdef CONFIG_MEMCG
	struct zone *zone;
#endif
};
#endif

/* Mask used at gathering information at once (see memcontrol.c) */
#define LRU_ALL_FILE (BIT(LRU_INACTIVE_FILE) | BIT(LRU_ACTIVE_FILE))
#define LRU_ALL_ANON (BIT(LRU_INACTIVE_ANON) | BIT(LRU_ACTIVE_ANON))
#define LRU_ALL	     ((1 << NR_LRU_LISTS) - 1)

/* Isolate clean file */
#define ISOLATE_CLEAN		((__force isolate_mode_t)0x1)
/* Isolate unmapped file */
#define ISOLATE_UNMAPPED	((__force isolate_mode_t)0x2)
/* Isolate for asynchronous migration */
#define ISOLATE_ASYNC_MIGRATE	((__force isolate_mode_t)0x4)
/* Isolate unevictable pages */
#define ISOLATE_UNEVICTABLE	((__force isolate_mode_t)0x8)

/* LRU Isolation modes. */
typedef unsigned __bitwise__ isolate_mode_t;

enum zone_watermarks {
	WMARK_MIN,
	WMARK_LOW,
	WMARK_HIGH,
	NR_WMARK
};

#define min_wmark_pages(z) (z->watermark[WMARK_MIN])
#define low_wmark_pages(z) (z->watermark[WMARK_LOW])
#define high_wmark_pages(z) (z->watermark[WMARK_HIGH])

struct per_cpu_pages {
	int count;		/* number of pages in the list */
	int high;		/* high watermark, emptying needed */
	int batch;		/* chunk size for buddy add/remove */

	/* Lists of pages, one per migrate type stored on the pcp-lists */
	struct list_head lists[MIGRATE_PCPTYPES];
};

struct per_cpu_pageset {
	struct per_cpu_pages pcp;
#ifdef CONFIG_NUMA
	s8 expire;
#endif
#ifdef CONFIG_SMP
	s8 stat_threshold;
	s8 vm_stat_diff[NR_VM_ZONE_STAT_ITEMS];
#endif
};

#endif /* !__GENERATING_BOUNDS.H */

enum zone_type {
#ifdef CONFIG_ZONE_DMA
	/*
	 * ZONE_DMA is used when there are devices that are not able
	 * to do DMA to all of addressable memory (ZONE_NORMAL). Then we
	 * carve out the portion of memory that is needed for these devices.
	 * The range is arch specific.
	 *
	 * Some examples
	 *
	 * Architecture		Limit
	 * ---------------------------
	 * parisc, ia64, sparc	<4G
	 * s390			<2G
	 * arm			Various
	 * alpha		Unlimited or 0-16MB.
	 *
	 * i386, x86_64 and multiple other arches
	 * 			<16M.
	 */
	ZONE_DMA,
#endif
#ifdef CONFIG_ZONE_DMA32
	/*
	 * x86_64 needs two ZONE_DMAs because it supports devices that are
	 * only able to do DMA to the lower 16M but also 32 bit devices that
	 * can only do DMA areas below 4G.
	 */
	ZONE_DMA32,
#endif
	/*
	 * Normal addressable memory is in ZONE_NORMAL. DMA operations can be
	 * performed on pages in ZONE_NORMAL if the DMA devices support
	 * transfers to all addressable memory.
	 */
	ZONE_NORMAL,
#ifdef CONFIG_HIGHMEM
	/*
	 * A memory area that is only addressable by the kernel through
	 * mapping portions into its own address space. This is for example
	 * used by i386 to allow the kernel to address the memory beyond
	 * 900MB. The kernel will set up special mappings (page
	 * table entries on i386) for each page that the kernel needs to
	 * access.
	 */
	ZONE_HIGHMEM,
#endif
	ZONE_MOVABLE,
	__MAX_NR_ZONES
};

#ifndef __GENERATING_BOUNDS_H

/*
 * When a memory allocation must conform to specific limitations (such
 * as being suitable for DMA) the caller will pass in hints to the
 * allocator in the gfp_mask, in the zone modifier bits.  These bits
 * are used to select a priority ordered list of memory zones which
 * match the requested limits. See gfp_zone() in include/linux/gfp.h
 */

#if MAX_NR_ZONES < 2
#define ZONES_SHIFT 0
#elif MAX_NR_ZONES <= 2
#define ZONES_SHIFT 1
#elif MAX_NR_ZONES <= 4
#define ZONES_SHIFT 2
#else
#error ZONES_SHIFT -- too many zones configured adjust calculation
#endif

struct zone {
	/* Fields commonly accessed by the page allocator */

	/* zone watermarks, access with *_wmark_pages(zone) macros */
	unsigned long watermark[NR_WMARK];

	/*
	 * When free pages are below this point, additional steps are taken
	 * when reading the number of free pages to avoid per-cpu counter
	 * drift allowing watermarks to be breached
	 */
	unsigned long percpu_drift_mark;

	/*
	 * We don't know if the memory that we're going to allocate will be freeable
	 * or/and it will be released eventually, so to avoid totally wasting several
	 * GB of ram we must reserve some of the lower zone memory (otherwise we risk
	 * to run OOM on the lower zones despite there's tons of freeable ram
	 * on the higher zones). This array is recalculated at runtime if the
	 * sysctl_lowmem_reserve_ratio sysctl changes.
	 */
	unsigned long		lowmem_reserve[MAX_NR_ZONES];

	/*
	 * This is a per-zone reserve of pages that should not be
	 * considered dirtyable memory.
	 */
	unsigned long		dirty_balance_reserve;

#ifdef CONFIG_NUMA
	int node;
	/*
	 * zone reclaim becomes active if more unmapped pages exist.
	 */
	unsigned long		min_unmapped_pages;
	unsigned long		min_slab_pages;
#endif
	struct per_cpu_pageset __percpu *pageset;
	/*
	 * free areas of different sizes
	 */
	spinlock_t		lock;
	int                     all_unreclaimable; /* All pages pinned - true if all are unrecl */
#if defined CONFIG_COMPACTION || defined CONFIG_CMA
	/* Set to true when the PG_migrate_skip bits should be cleared */
	bool			compact_blockskip_flush;

	/* pfns where compaction scanners should start */
	unsigned long		compact_cached_free_pfn;
	unsigned long		compact_cached_migrate_pfn;
#endif
#ifdef CONFIG_MEMORY_HOTPLUG
	/* see spanned/present_pages for more description */
	seqlock_t		span_seqlock;
#endif
	struct free_area	free_area[MAX_ORDER];

#ifndef CONFIG_SPARSEMEM
	/*
	 * Flags for a pageblock_nr_pages block. See pageblock-flags.h.
	 * In SPARSEMEM, this map is stored in struct mem_section
	 */
	unsigned long		*pageblock_flags;
#endif /* CONFIG_SPARSEMEM */

#ifdef CONFIG_COMPACTION
	/*
	 * On compaction failure, 1<<compact_defer_shift compactions
	 * are skipped before trying again. The number attempted since
	 * last failure is tracked with compact_considered.
	 */
	unsigned int		compact_considered;
	unsigned int		compact_defer_shift;
	int			compact_order_failed;
#endif

	ZONE_PADDING(_pad1_)

	/* Fields commonly accessed by the page reclaim scanner */
	spinlock_t		lru_lock;
#ifdef CONFIG_RECLAIM_POLICY
	struct lruvec			*lruvecs[NR_PAGE_RECLAIM_POLICIES];
	/* The reclaim policy of the zone. */
	enum pgreclaim_policy_list	policy;
	struct list_head history[NR_HISTORY_LISTS];
#else
	struct lruvec		lruvec;
#endif

	unsigned long		pages_scanned;	   /* since last reclaim */
	unsigned long		flags;		   /* zone flags, see below */

	/* Zone statistics */
	atomic_long_t		vm_stat[NR_VM_ZONE_STAT_ITEMS];

	/*
	 * The target ratio of ACTIVE_ANON to INACTIVE_ANON pages on
	 * this zone's LRU.  Maintained by the pageout code.
	 */
	unsigned int inactive_ratio;


	ZONE_PADDING(_pad2_)
	/* Rarely used or read-mostly fields */

	/*
	 * wait_table		-- the array holding the hash table
	 * wait_table_hash_nr_entries	-- the size of the hash table array
	 * wait_table_bits	-- wait_table_size == (1 << wait_table_bits)
	 *
	 * The purpose of all these is to keep track of the people
	 * waiting for a page to become available and make them
	 * runnable again when possible. The trouble is that this
	 * consumes a lot of space, especially when so few things
	 * wait on pages at a given time. So instead of using
	 * per-page waitqueues, we use a waitqueue hash table.
	 *
	 * The bucket discipline is to sleep on the same queue when
	 * colliding and wake all in that wait queue when removing.
	 * When something wakes, it must check to be sure its page is
	 * truly available, a la thundering herd. The cost of a
	 * collision is great, but given the expected load of the
	 * table, they should be so rare as to be outweighed by the
	 * benefits from the saved space.
	 *
	 * __wait_on_page_locked() and unlock_page() in mm/filemap.c, are the
	 * primary users of these fields, and in mm/page_alloc.c
	 * free_area_init_core() performs the initialization of them.
	 */
	wait_queue_head_t	* wait_table;
	unsigned long		wait_table_hash_nr_entries;
	unsigned long		wait_table_bits;

	/*
	 * Discontig memory support fields.
	 */
	struct pglist_data	*zone_pgdat;
	/* zone_start_pfn == zone_start_paddr >> PAGE_SHIFT */
	unsigned long		zone_start_pfn;

	/*
	 * spanned_pages is the total pages spanned by the zone, including
	 * holes, which is calculated as:
	 * 	spanned_pages = zone_end_pfn - zone_start_pfn;
	 *
	 * present_pages is physical pages existing within the zone, which
	 * is calculated as:
	 *	present_pages = spanned_pages - absent_pages(pags in holes);
	 *
	 * managed_pages is present pages managed by the buddy system, which
	 * is calculated as (reserved_pages includes pages allocated by the
	 * bootmem allocator):
	 *	managed_pages = present_pages - reserved_pages;
	 *
	 * So present_pages may be used by memory hotplug or memory power
	 * management logic to figure out unmanaged pages by checking
	 * (present_pages - managed_pages). And managed_pages should be used
	 * by page allocator and vm scanner to calculate all kinds of watermarks
	 * and thresholds.
	 *
	 * Locking rules:
	 *
	 * zone_start_pfn and spanned_pages are protected by span_seqlock.
	 * It is a seqlock because it has to be read outside of zone->lock,
	 * and it is done in the main allocator path.  But, it is written
	 * quite infrequently.
	 *
	 * The span_seq lock is declared along with zone->lock because it is
	 * frequently read in proximity to zone->lock.  It's good to
	 * give them a chance of being in the same cacheline.
	 *
	 * Write access to present_pages and managed_pages at runtime should
	 * be protected by lock_memory_hotplug()/unlock_memory_hotplug().
	 * Any reader who can't tolerant drift of present_pages and
	 * managed_pages should hold memory hotplug lock to get a stable value.
	 */
	unsigned long		spanned_pages;
	unsigned long		present_pages;
	unsigned long		managed_pages;

	/*
	 * rarely used fields:
	 */
	const char		*name;
} ____cacheline_internodealigned_in_smp;

typedef enum {
	ZONE_RECLAIM_LOCKED,		/* prevents concurrent reclaim */
	ZONE_OOM_LOCKED,		/* zone is in OOM killer zonelist */
	ZONE_CONGESTED,			/* zone has many dirty pages backed by
					 * a congested BDI
					 */
} zone_flags_t;

static inline void zone_set_flag(struct zone *zone, zone_flags_t flag)
{
	set_bit(flag, &zone->flags);
}

static inline int zone_test_and_set_flag(struct zone *zone, zone_flags_t flag)
{
	return test_and_set_bit(flag, &zone->flags);
}

static inline void zone_clear_flag(struct zone *zone, zone_flags_t flag)
{
	clear_bit(flag, &zone->flags);
}

static inline int zone_is_reclaim_congested(const struct zone *zone)
{
	return test_bit(ZONE_CONGESTED, &zone->flags);
}

static inline int zone_is_reclaim_locked(const struct zone *zone)
{
	return test_bit(ZONE_RECLAIM_LOCKED, &zone->flags);
}

static inline int zone_is_oom_locked(const struct zone *zone)
{
	return test_bit(ZONE_OOM_LOCKED, &zone->flags);
}



//PETER - Start
#ifdef CONFIG_RECLAIM_POLICY
enum page_references {
	PAGEREF_RECLAIM,
	PAGEREF_RECLAIM_CLEAN,
	PAGEREF_KEEP,
	PAGEREF_ACTIVATE,
};

struct scan_control {
	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;

	/* How many pages shrink_list() should reclaim */
	unsigned long nr_to_reclaim;

	unsigned long hibernation_mode;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	int may_writepage;

	/* Can mapped pages be reclaimed? */
	int may_unmap;

	/* Can pages be swapped as part of reclaim? */
	int may_swap;

	int order;

	/* Scan (total_size >> priority) pages at once */
	int priority;

	/*
	 * The memory cgroup that hit its limit and as a result is the
	 * primary target of this reclaim invocation.
	 */
	struct mem_cgroup *target_mem_cgroup;

	/*
	 * Nodemask of nodes allowed by the caller. If NULL, all nodes
	 * are scanned.
	 */
	nodemask_t	*nodemask;
};


/*
 * Important variables related to the page replacement algorithm:
 * extern long nr_swap_pages - Number of pages to swap, if > 0 swap enabled
 * extern long total_swap_pages - Total number of swap pages, if == 0 swap
 * 				  disabled
 *
 * Class to implement different page replacement algorithms.
 */
struct page_reclaim_policy {
	/*
	 * Initialize memory using the bootmem allocator. Init the lruvec
	 * struct.
	 */
	void (*init_lruvec) (struct lruvec **lruvec, struct zone *zone);
	/* 
	 * Core function used when srhinking the lruvec lists. It populates the
	 * nr array that indicate how many pages will be scanned and shrinked
	 * by the shrink_lruvec function. Called before shrink_lruvec.
	 */
	void (*get_scan_count) (struct lruvec *lruvec, struct scan_control *sc,
				unsigned long *nr, bool force_scan);
	/*
	 * Actually shrinks the lruvec lists and reclaims pages.
	 */
	int (*shrink_lruvec) (struct lruvec *lruvec, struct scan_control *sc,
			      unsigned long *nr);
	/*
	 * Balances the lruvec lists if the algorithm uses more than one list
	 * for evictable pages and moves pages among the lists.
	 * Leave the function empty, if balancing among lists is not needed
	 * (e.g. FIFO).
	 * In the LRU2Q, the default algorithm, this function balances the
	 * active and inactive lists to keep the correct ratio.
	 */
	void (*balance_lruvec) (struct lruvec *lruvec, struct scan_control *sc);
	/*
	 * Test to see if we should continue reclaim.
	 * The implementation logic of this function is to check if there are
	 * enough pages for freeing from the lists than the number of pages that
	 * are needed for compaction. In that case return true. Otherwise false.
	 * Of course depending on the reclaim algorithm, it might be decided to
	 * continue reclaim on other conditions.
	 */
	bool (*should_continue_reclaim) (struct lruvec *lruvec,
					 unsigned long nr_reclaimed,
					 unsigned long nr_scanned,
					 struct scan_control *sc);
	/*
	 * Very important function in determining how many pages will be shrank
	 * during the shrink_lruvec call. Used for measuring how many pages to
	 * shrink.
	 * This function should return the number of pages in the lruvec lists
	 * that can be reclaimed. The number of evictable pages should be
	 * returned. If swapping is disabled the number of anonymous pages
	 * should be excluded, only file mapped pages should be retruned.
	 */
	unsigned long (*zone_reclaimable_pages) (struct zone *zone);
	/*
	 * The same as zone_reclaimable pages but this time for all pages in all
	 * the zones.
	 */
	unsigned long (*global_reclaimable_pages) (void);
	/*
	 * Return a bool value that indicates that we have isolated enough
	 * pages for compaction to go ahead and compact free pages, and satisfy
	 * the page_allocation request.
	 *
	 * This function should compare the number of isolated pages to the
	 * number of pages that are evictable/reclaimable and return true if
	 * more pages are isolated than there are actually on the lruvec lists.
	 */
	bool (*too_many_isolated_compaction) (struct zone *zone);
	/*
	 * Mark a page as having seen activity. Test the flags of the page to
	 * determine if it referenced and active. It can be in one of the
	 * following states:
	 * inactive,unreferenced
	 * inactive,referenced
	 * active,unreferenced
	 * active,referenced
	 * called in mm/swap.c
	 * Good for capturing hit ratio of a cache.
	 */
	void (*page_accessed) (struct page *page);
	/* 
	 * Make a page active -- essentially move from inactive to active list.
	 * A page has become "active" its "PG_active" flag must be set to 1.
	 * The SetPageActive(page) needs to be called to set the active flag.
	 * In the default reclaim_policy the page is deleted from the inactive
	 * lru list and its placed on the active list. The SetPageActive method
	 * is called after deleting the page from the inactive list and before
	 * putting it on the active list.
	 *
	 */
	void (*activate_page) (struct page *page, struct lruvec *lruvec);
	/*
	 * Make a page inactive -- essentially move from active to inactive list.
	 * The PG_referenced and PG_active flags must be cleared. The page is
	 * deleted from the inactive list and added to the active list.
	 * The page_writeback_or_dirty flag indicates whether we should mark
	 * the PG_reclaim flag or otherwise move the page to the tail of the
	 * inactive list.
	 */
	void (*deactivate_page) (struct page *page, struct lruvec *lruvec,
				 bool page_writeback_or_dirty);
	/*
	 * Update page reclaim statistics. This function is called when:
	 *  i) a page is activated or forcefully deactivated;
	 * ii) a page is directly added to the lruvec lists.
	 *
	 * The default (LRU2Q) page rep. algorithm uses this function to account
	 * the number of pages that were scanned for reclaim or added, and the
	 * number of active pages moved from the inactive list to the active, or
	 * the number of active pages added to the lruvec lists.
	 * Other page rep. algorithms might not need this function at all and
	 * can leave it empty.
	 *
	 * @type - can be either 0 for anon pages, or 1 for file pages.
	 * @active - 0: the page is inactive or is being deactivated;
	 *	     1: the page is active or being activated;
	 */
	void (*update_reclaim_statistics) (struct lruvec *lruvec, int type,
					   int active);
	/*
	 * Actually adds a page to the lruvec list.
	 * !! Maybe it should be united with add_page and pass a
	 * a parameter indicating whether the function is called for the first
	 * time.
	 */
	void (*add_page_to_list) (struct page *page, struct lruvec *lruvec,
				  int list_index);
	/*
	 * Actually removes a page from the lruvec list.
	 * !! Maybe it should be united with del_page_from_list and pass a
	 * parameter to indicate that the function was called to release
	 * (reclaim) the pages without using the page replacement algorithm.
	 */
	void (*del_page_from_list) (struct page *page, struct lruvec *lruvec,
				    int list_index);
	/*
	 * First time a page is added to any of the lruvec lists. Check the lru
	 * list to see where the page is to be added e.g. it can be added to the
	 * unevictable list, and this is a statistic we want to take into
	 * consideration when making decision about which reclaim_policy to use.
	 *
	 * Internally this function should call/do the same thing as
	 * add_page_to_list. However the difference between them is that this
	 * function is called only in the case when a new page has been just
	 * added to the lruvec lists for the _first_ time. Thus this can be
	 * used for accounting statistics when deciding which page replacement
	 * algorithm to use.
	 * !! Maybe it should be united with add_page_to_list and pass
	 * a parameter indicating whether the function is called for the first
	 * time.
	 */
	void (*add_page) (struct page *page, struct lruvec *lruvec, int lru);
	/*
	 * Removes pages directly from the lru list - does not wait to shrink
	 * the inactive lists. The idea is to free pages that are cache hot.
	 * Called in two places in mm/swap.c -> __page_cache_release and
	 * release_pages.
	 *
	 * Internally this functions should call/do the same thing as
	 * del_page_from_list. However the difference between them is that this
	 * function is called only in the case when the pages are directly
	 * reclaimed without using the page replcement algorithm at all.
	 * The function is called only in mm/swap.c.
	 * !! Maybe it should be united with del_page_from_list and pass a
	 * parameter to indicate that the function was called to release
	 * (reclaim) the pages without using the page replacement algorithm.
	 */
	void (*release_page) (struct page *page, struct lruvec *lruvec, int lru,
			      bool batch_release);
	/*
	 * LRU2Q:
	 * This function puts an inactive page back to the inactive list. It
	 * puts the page on the tail of the inactive list, which means it will
	 * be reclaimed shortly. It's a good place
	 * to track pages which have been tried to be reclaimed, but there was
	 * something preventing this to happen. (accounting purposes)
	 * Other page rep. algorithms can use this function for accounting
	 * purposes or to put a page back to the lruvec lists, but put it such
	 * that it should be reclaimed in the next round for reclaim. (e.g. FIFO
	 * should place it among the first pages in the list for reclaim).
	 */
	void (*rotate_inactive_page) (struct page *page, struct lruvec *lruvec);
	/*
	 * This function should return a pointer to the respective lruvec list,
	 * based on the lru type of the page passed. This is needed in
	 * mm/memcontrol.c to empty the specified lruvec list, and the caller
	 * will move all the pages to the parent mem_cgroup. The function is
	 * called for every possible lru (INACTIVE_ANON, ACTIVE_ANON,
	 * INACTIVE_FILE, ACTIVE_FILE, UNEVICTABLE).
	 * The caller of this function goes through each list starting from the
	 * tail and going to the head, this might destroy the recency
	 * information of some of the pages, if the pages are not ordered in
	 * this specific way, i.e. most recent pages on the head, and least
	 * recent pages on the tail.
	 */
	struct list_head* (*get_lruvec_list) (struct lruvec *lruvec, int lru);
	/*
	 * Clear (reset) the statistics for the number of pages currently 
	 * held into memory by the current zone. This function should be invoked
	 * for the old reclaim_policy when switching to new reclaim policy.
	 * @zone The zone whoose vmstat will be cleared.
	 * @evictable True - clear statistics for evictable lists.
	 *            False - clear statistics for unevictable lists.
	 */
	void (*reset_zone_vmstat) (struct lruvec *lruvec, struct zone *zone,
				   bool evictable);

	/*
	 * For testing purposes
	 */
	struct list_head* (*get_list) (struct lruvec *lruvec, int lru);
	void (*print_lruvec) (struct zone *zone);

	void (*nonres_remember) (struct zone *zone, struct page *page);
	void (*add_page_unevictable) (struct page *page, struct lruvec *lruvec);
	void (*isolate) (struct page *page, struct lruvec *lruvec);
	void (*putback_page) (struct page *page);
	void (*nonres_forget) (struct address_space *mapping, unsigned long index);
	/*
	 * Check page reference flags and decide what should we do with the
	 * page: a) We can activate it; b) We can reclaim it; c) We can keep it
	 * locked; d) We can move it to the unevictable list;
	 */
	int (*page_check_references) (struct page *page, struct scan_control *sc);
	/*
	 * This function should set the flags to mark the page as active and
	 * return 'true' if the page was activated (i.e. changed it status from
	 * inactive to active) and return 'false' if the page was already active
	 * and no change was done.
	 * In the LRU-2Q it only sets the page's Active flag.
	 * In Clock-Pro it sets the page as Test or Hot.
	 */
	int (*activate) (struct page *page);

	/*
	 * This function is called when a new page is added to page cache.
	 * It will set the correct flags for the page.
	 * */
/*
#ifdef CONFIG_MEMCG
	struct scan_control (*get_scan_control_memcg) (void);
#endif
*/
};
/* Currently implemented page_reclaim_policies */
extern const struct page_reclaim_policy lru_page_reclaim_policy;  /* mm/lru.c */
extern const struct page_reclaim_policy fifo_page_reclaim_policy; /* mm/fifo.c */
extern const struct page_reclaim_policy clockpro_page_reclaim_policy; /* mm/clock-pro.c */
extern const struct page_reclaim_policy arc_page_reclaim_policy; /* mm/fifo.c */
extern const struct page_reclaim_policy cart_page_reclaim_policy; /* mm/clock-pro.c */
/* mm/mmzone.c */
extern void init_lruvecs(struct lruvec *lruvecs[], struct zone *zone,
			 struct mem_cgroup_per_zone *mz);
extern void init_page_reclaim_policies(void);
extern void init_history_lists(struct list_head *history_list);
extern int switch_reclaim_policy(struct zone *zone,
				 enum pgreclaim_policy_list old_p,
				 enum pgreclaim_policy_list new_p);
extern int switch_reclaim_policy_all(enum pgreclaim_policy_list old_p,
				     enum pgreclaim_policy_list new_p);
/* mm/history.c */
extern
void add_to_history(struct page *page, struct list_head *history, int index);
extern
void delete_from_history(struct page *page, struct list_head *history, int index);
extern
void rotate_page_history(struct page *page, struct list_head *history, int index);
extern int get_history_index(enum lru_list lru);
/* linux/mm/vmscan.c */
extern bool global_reclaim(struct scan_control *sc);
extern unsigned long shrink_page_list(struct list_head *page_list,
				      struct zone *zone,
				      struct scan_control *sc,
				      int ttu_flags,
				      unsigned long *ret_nr_dirty,
				      unsigned long *ret_nr_writeback,
				      bool force_reclaim);
/* mm/mmzone.c */
extern struct zone *lruvec_zone(struct lruvec *lruvec);
extern void set_lruvec_zone(struct lruvec *lruvec, struct zone *zone);
extern struct mem_cgroup_per_zone *lruvec_mz(struct lruvec *lruvec);
extern void set_lruvec_mz(struct lruvec *lruvec, struct mem_cgroup_per_zone *mz);

/* linux/mm/lru.c */
extern struct list_head *get_lruvec_list(struct lruvec *lruvec, int lru);
extern struct zone_reclaim_stat *get_reclaim_stat(struct lruvec *lruvec);
extern unsigned long get_scanned(struct zone_reclaim_stat *reclaim_stat, int);
extern unsigned long get_rotated(struct zone_reclaim_stat *reclaim_stat, int);
extern void update_scanned(struct zone_reclaim_stat *reclaim_stat, int lru,
			   unsigned long n);
extern void update_rotated(struct zone_reclaim_stat *reclaim_stat, int lru,
			   unsigned long n);

#endif /* CONFIG_RECLAIM_POLICY */
//PETER - End



/*
 * The "priority" of VM scanning is how much of the queues we will scan in one
 * go. A value of 12 for DEF_PRIORITY implies that we will scan 1/4096th of the
 * queues ("queue_length >> 12") during an aging round.
 */
#define DEF_PRIORITY 12

/* Maximum number of zones on a zonelist */
#define MAX_ZONES_PER_ZONELIST (MAX_NUMNODES * MAX_NR_ZONES)

#ifdef CONFIG_NUMA

/*
 * The NUMA zonelists are doubled because we need zonelists that restrict the
 * allocations to a single node for GFP_THISNODE.
 *
 * [0]	: Zonelist with fallback
 * [1]	: No fallback (GFP_THISNODE)
 */
#define MAX_ZONELISTS 2


/*
 * We cache key information from each zonelist for smaller cache
 * footprint when scanning for free pages in get_page_from_freelist().
 *
 * 1) The BITMAP fullzones tracks which zones in a zonelist have come
 *    up short of free memory since the last time (last_fullzone_zap)
 *    we zero'd fullzones.
 * 2) The array z_to_n[] maps each zone in the zonelist to its node
 *    id, so that we can efficiently evaluate whether that node is
 *    set in the current tasks mems_allowed.
 *
 * Both fullzones and z_to_n[] are one-to-one with the zonelist,
 * indexed by a zones offset in the zonelist zones[] array.
 *
 * The get_page_from_freelist() routine does two scans.  During the
 * first scan, we skip zones whose corresponding bit in 'fullzones'
 * is set or whose corresponding node in current->mems_allowed (which
 * comes from cpusets) is not set.  During the second scan, we bypass
 * this zonelist_cache, to ensure we look methodically at each zone.
 *
 * Once per second, we zero out (zap) fullzones, forcing us to
 * reconsider nodes that might have regained more free memory.
 * The field last_full_zap is the time we last zapped fullzones.
 *
 * This mechanism reduces the amount of time we waste repeatedly
 * reexaming zones for free memory when they just came up low on
 * memory momentarilly ago.
 *
 * The zonelist_cache struct members logically belong in struct
 * zonelist.  However, the mempolicy zonelists constructed for
 * MPOL_BIND are intentionally variable length (and usually much
 * shorter).  A general purpose mechanism for handling structs with
 * multiple variable length members is more mechanism than we want
 * here.  We resort to some special case hackery instead.
 *
 * The MPOL_BIND zonelists don't need this zonelist_cache (in good
 * part because they are shorter), so we put the fixed length stuff
 * at the front of the zonelist struct, ending in a variable length
 * zones[], as is needed by MPOL_BIND.
 *
 * Then we put the optional zonelist cache on the end of the zonelist
 * struct.  This optional stuff is found by a 'zlcache_ptr' pointer in
 * the fixed length portion at the front of the struct.  This pointer
 * both enables us to find the zonelist cache, and in the case of
 * MPOL_BIND zonelists, (which will just set the zlcache_ptr to NULL)
 * to know that the zonelist cache is not there.
 *
 * The end result is that struct zonelists come in two flavors:
 *  1) The full, fixed length version, shown below, and
 *  2) The custom zonelists for MPOL_BIND.
 * The custom MPOL_BIND zonelists have a NULL zlcache_ptr and no zlcache.
 *
 * Even though there may be multiple CPU cores on a node modifying
 * fullzones or last_full_zap in the same zonelist_cache at the same
 * time, we don't lock it.  This is just hint data - if it is wrong now
 * and then, the allocator will still function, perhaps a bit slower.
 */


struct zonelist_cache {
	unsigned short z_to_n[MAX_ZONES_PER_ZONELIST];		/* zone->nid */
	DECLARE_BITMAP(fullzones, MAX_ZONES_PER_ZONELIST);	/* zone full? */
	unsigned long last_full_zap;		/* when last zap'd (jiffies) */
};
#else
#define MAX_ZONELISTS 1
struct zonelist_cache;
#endif

/*
 * This struct contains information about a zone in a zonelist. It is stored
 * here to avoid dereferences into large structures and lookups of tables
 */
struct zoneref {
	struct zone *zone;	/* Pointer to actual zone */
	int zone_idx;		/* zone_idx(zoneref->zone) */
};

/*
 * One allocation request operates on a zonelist. A zonelist
 * is a list of zones, the first one is the 'goal' of the
 * allocation, the other zones are fallback zones, in decreasing
 * priority.
 *
 * If zlcache_ptr is not NULL, then it is just the address of zlcache,
 * as explained above.  If zlcache_ptr is NULL, there is no zlcache.
 * *
 * To speed the reading of the zonelist, the zonerefs contain the zone index
 * of the entry being read. Helper functions to access information given
 * a struct zoneref are
 *
 * zonelist_zone()	- Return the struct zone * for an entry in _zonerefs
 * zonelist_zone_idx()	- Return the index of the zone for an entry
 * zonelist_node_idx()	- Return the index of the node for an entry
 */
struct zonelist {
	struct zonelist_cache *zlcache_ptr;		     // NULL or &zlcache
	struct zoneref _zonerefs[MAX_ZONES_PER_ZONELIST + 1];
#ifdef CONFIG_NUMA
	struct zonelist_cache zlcache;			     // optional ...
#endif
};

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
struct node_active_region {
	unsigned long start_pfn;
	unsigned long end_pfn;
	int nid;
};
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

#ifndef CONFIG_DISCONTIGMEM
/* The array of struct pages - for discontigmem use pgdat->lmem_map */
extern struct page *mem_map;
#endif

/*
 * The pg_data_t structure is used in machines with CONFIG_DISCONTIGMEM
 * (mostly NUMA machines?) to denote a higher-level memory zone than the
 * zone denotes.
 *
 * On NUMA machines, each NUMA node would have a pg_data_t to describe
 * it's memory layout.
 *
 * Memory statistics and page replacement data structures are maintained on a
 * per-zone basis.
 */
struct bootmem_data;
typedef struct pglist_data {
	struct zone node_zones[MAX_NR_ZONES];
	struct zonelist node_zonelists[MAX_ZONELISTS];
	int nr_zones;
#ifdef CONFIG_FLAT_NODE_MEM_MAP	/* means !SPARSEMEM */
	struct page *node_mem_map;
#ifdef CONFIG_MEMCG
	struct page_cgroup *node_page_cgroup;
#endif
#endif
#ifndef CONFIG_NO_BOOTMEM
	struct bootmem_data *bdata;
#endif
#ifdef CONFIG_MEMORY_HOTPLUG
	/*
	 * Must be held any time you expect node_start_pfn, node_present_pages
	 * or node_spanned_pages stay constant.  Holding this will also
	 * guarantee that any pfn_valid() stays that way.
	 *
	 * Nests above zone->lock and zone->size_seqlock.
	 */
	spinlock_t node_size_lock;
#endif
	unsigned long node_start_pfn;
	unsigned long node_present_pages; /* total number of physical pages */
	unsigned long node_spanned_pages; /* total size of physical page
					     range, including holes */
	int node_id;
	nodemask_t reclaim_nodes;	/* Nodes allowed to reclaim from */
	wait_queue_head_t kswapd_wait;
	wait_queue_head_t pfmemalloc_wait;
	struct task_struct *kswapd;	/* Protected by lock_memory_hotplug() */
	int kswapd_max_order;
	enum zone_type classzone_idx;
#ifdef CONFIG_NUMA_BALANCING
	/*
	 * Lock serializing the per destination node AutoNUMA memory
	 * migration rate limiting data.
	 */
	spinlock_t numabalancing_migrate_lock;

	/* Rate limiting time interval */
	unsigned long numabalancing_migrate_next_window;

	/* Number of pages migrated during the rate limiting time interval */
	unsigned long numabalancing_migrate_nr_pages;
#endif
} pg_data_t;

#define node_present_pages(nid)	(NODE_DATA(nid)->node_present_pages)
#define node_spanned_pages(nid)	(NODE_DATA(nid)->node_spanned_pages)
#ifdef CONFIG_FLAT_NODE_MEM_MAP
#define pgdat_page_nr(pgdat, pagenr)	((pgdat)->node_mem_map + (pagenr))
#else
#define pgdat_page_nr(pgdat, pagenr)	pfn_to_page((pgdat)->node_start_pfn + (pagenr))
#endif
#define nid_page_nr(nid, pagenr) 	pgdat_page_nr(NODE_DATA(nid),(pagenr))

#define node_start_pfn(nid)	(NODE_DATA(nid)->node_start_pfn)

#define node_end_pfn(nid) ({\
	pg_data_t *__pgdat = NODE_DATA(nid);\
	__pgdat->node_start_pfn + __pgdat->node_spanned_pages;\
})

#include <linux/memory_hotplug.h>

extern struct mutex zonelists_mutex;
void build_all_zonelists(pg_data_t *pgdat, struct zone *zone);
void wakeup_kswapd(struct zone *zone, int order, enum zone_type classzone_idx);
bool zone_watermark_ok(struct zone *z, int order, unsigned long mark,
		int classzone_idx, int alloc_flags);
bool zone_watermark_ok_safe(struct zone *z, int order, unsigned long mark,
		int classzone_idx, int alloc_flags);
enum memmap_context {
	MEMMAP_EARLY,
	MEMMAP_HOTPLUG,
};
extern int init_currently_empty_zone(struct zone *zone, unsigned long start_pfn,
				     unsigned long size,
				     enum memmap_context context);

#ifndef CONFIG_RECLAIM_POLICY
extern void lruvec_init(struct lruvec *lruvec);
static inline struct zone *lruvec_zone(struct lruvec *lruvec)
{
#ifdef CONFIG_MEMCG
	return lruvec->zone;
#else
	return container_of(lruvec, struct zone, lruvec);
#endif
}
#endif

#ifdef CONFIG_HAVE_MEMORY_PRESENT
void memory_present(int nid, unsigned long start, unsigned long end);
#else
static inline void memory_present(int nid, unsigned long start, unsigned long end) {}
#endif

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
int local_memory_node(int node_id);
#else
static inline int local_memory_node(int node_id) { return node_id; };
#endif

#ifdef CONFIG_NEED_NODE_MEMMAP_SIZE
unsigned long __init node_memmap_size_bytes(int, unsigned long, unsigned long);
#endif

/*
 * zone_idx() returns 0 for the ZONE_DMA zone, 1 for the ZONE_NORMAL zone, etc.
 */
#define zone_idx(zone)		((zone) - (zone)->zone_pgdat->node_zones)

static inline int populated_zone(struct zone *zone)
{
	return (!!zone->present_pages);
}

extern int movable_zone;

static inline int zone_movable_is_highmem(void)
{
#if defined(CONFIG_HIGHMEM) && defined(CONFIG_HAVE_MEMBLOCK_NODE_MAP)
	return movable_zone == ZONE_HIGHMEM;
#else
	return 0;
#endif
}

static inline int is_highmem_idx(enum zone_type idx)
{
#ifdef CONFIG_HIGHMEM
	return (idx == ZONE_HIGHMEM ||
		(idx == ZONE_MOVABLE && zone_movable_is_highmem()));
#else
	return 0;
#endif
}

static inline int is_normal_idx(enum zone_type idx)
{
	return (idx == ZONE_NORMAL);
}

/**
 * is_highmem - helper function to quickly check if a struct zone is a 
 *              highmem zone or not.  This is an attempt to keep references
 *              to ZONE_{DMA/NORMAL/HIGHMEM/etc} in general code to a minimum.
 * @zone - pointer to struct zone variable
 */
static inline int is_highmem(struct zone *zone)
{
#ifdef CONFIG_HIGHMEM
	int zone_off = (char *)zone - (char *)zone->zone_pgdat->node_zones;
	return zone_off == ZONE_HIGHMEM * sizeof(*zone) ||
	       (zone_off == ZONE_MOVABLE * sizeof(*zone) &&
		zone_movable_is_highmem());
#else
	return 0;
#endif
}

static inline int is_normal(struct zone *zone)
{
	return zone == zone->zone_pgdat->node_zones + ZONE_NORMAL;
}

static inline int is_dma32(struct zone *zone)
{
#ifdef CONFIG_ZONE_DMA32
	return zone == zone->zone_pgdat->node_zones + ZONE_DMA32;
#else
	return 0;
#endif
}

static inline int is_dma(struct zone *zone)
{
#ifdef CONFIG_ZONE_DMA
	return zone == zone->zone_pgdat->node_zones + ZONE_DMA;
#else
	return 0;
#endif
}

/* These two functions are used to setup the per zone pages min values */
struct ctl_table;
int min_free_kbytes_sysctl_handler(struct ctl_table *, int,
					void __user *, size_t *, loff_t *);
extern int sysctl_lowmem_reserve_ratio[MAX_NR_ZONES-1];
int lowmem_reserve_ratio_sysctl_handler(struct ctl_table *, int,
					void __user *, size_t *, loff_t *);
int percpu_pagelist_fraction_sysctl_handler(struct ctl_table *, int,
					void __user *, size_t *, loff_t *);
int sysctl_min_unmapped_ratio_sysctl_handler(struct ctl_table *, int,
			void __user *, size_t *, loff_t *);
int sysctl_min_slab_ratio_sysctl_handler(struct ctl_table *, int,
			void __user *, size_t *, loff_t *);

extern int numa_zonelist_order_handler(struct ctl_table *, int,
			void __user *, size_t *, loff_t *);
extern char numa_zonelist_order[];
#define NUMA_ZONELIST_ORDER_LEN 16	/* string buffer size */

#ifndef CONFIG_NEED_MULTIPLE_NODES

extern struct pglist_data contig_page_data;
#define NODE_DATA(nid)		(&contig_page_data)
#define NODE_MEM_MAP(nid)	mem_map

#else /* CONFIG_NEED_MULTIPLE_NODES */

#include <asm/mmzone.h>

#endif /* !CONFIG_NEED_MULTIPLE_NODES */

extern struct pglist_data *first_online_pgdat(void);
extern struct pglist_data *next_online_pgdat(struct pglist_data *pgdat);
extern struct zone *next_zone(struct zone *zone);

/**
 * for_each_online_pgdat - helper macro to iterate over all online nodes
 * @pgdat - pointer to a pg_data_t variable
 */
#define for_each_online_pgdat(pgdat)			\
	for (pgdat = first_online_pgdat();		\
	     pgdat;					\
	     pgdat = next_online_pgdat(pgdat))
/**
 * for_each_zone - helper macro to iterate over all memory zones
 * @zone - pointer to struct zone variable
 *
 * The user only needs to declare the zone variable, for_each_zone
 * fills it in.
 */
#define for_each_zone(zone)			        \
	for (zone = (first_online_pgdat())->node_zones; \
	     zone;					\
	     zone = next_zone(zone))

#define for_each_populated_zone(zone)		        \
	for (zone = (first_online_pgdat())->node_zones; \
	     zone;					\
	     zone = next_zone(zone))			\
		if (!populated_zone(zone))		\
			; /* do nothing */		\
		else

static inline struct zone *zonelist_zone(struct zoneref *zoneref)
{
	return zoneref->zone;
}

static inline int zonelist_zone_idx(struct zoneref *zoneref)
{
	return zoneref->zone_idx;
}

static inline int zonelist_node_idx(struct zoneref *zoneref)
{
#ifdef CONFIG_NUMA
	/* zone_to_nid not available in this context */
	return zoneref->zone->node;
#else
	return 0;
#endif /* CONFIG_NUMA */
}

/**
 * next_zones_zonelist - Returns the next zone at or below highest_zoneidx within the allowed nodemask using a cursor within a zonelist as a starting point
 * @z - The cursor used as a starting point for the search
 * @highest_zoneidx - The zone index of the highest zone to return
 * @nodes - An optional nodemask to filter the zonelist with
 * @zone - The first suitable zone found is returned via this parameter
 *
 * This function returns the next zone at or below a given zone index that is
 * within the allowed nodemask using a cursor as the starting point for the
 * search. The zoneref returned is a cursor that represents the current zone
 * being examined. It should be advanced by one before calling
 * next_zones_zonelist again.
 */
struct zoneref *next_zones_zonelist(struct zoneref *z,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes,
					struct zone **zone);

/**
 * first_zones_zonelist - Returns the first zone at or below highest_zoneidx within the allowed nodemask in a zonelist
 * @zonelist - The zonelist to search for a suitable zone
 * @highest_zoneidx - The zone index of the highest zone to return
 * @nodes - An optional nodemask to filter the zonelist with
 * @zone - The first suitable zone found is returned via this parameter
 *
 * This function returns the first zone at or below a given zone index that is
 * within the allowed nodemask. The zoneref returned is a cursor that can be
 * used to iterate the zonelist with next_zones_zonelist by advancing it by
 * one before calling.
 */
static inline struct zoneref *first_zones_zonelist(struct zonelist *zonelist,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes,
					struct zone **zone)
{
	return next_zones_zonelist(zonelist->_zonerefs, highest_zoneidx, nodes,
								zone);
}

/**
 * for_each_zone_zonelist_nodemask - helper macro to iterate over valid zones in a zonelist at or below a given zone index and within a nodemask
 * @zone - The current zone in the iterator
 * @z - The current pointer within zonelist->zones being iterated
 * @zlist - The zonelist being iterated
 * @highidx - The zone index of the highest zone to return
 * @nodemask - Nodemask allowed by the allocator
 *
 * This iterator iterates though all zones at or below a given zone index and
 * within a given nodemask
 */
#define for_each_zone_zonelist_nodemask(zone, z, zlist, highidx, nodemask) \
	for (z = first_zones_zonelist(zlist, highidx, nodemask, &zone);	\
		zone;							\
		z = next_zones_zonelist(++z, highidx, nodemask, &zone))	\

/**
 * for_each_zone_zonelist - helper macro to iterate over valid zones in a zonelist at or below a given zone index
 * @zone - The current zone in the iterator
 * @z - The current pointer within zonelist->zones being iterated
 * @zlist - The zonelist being iterated
 * @highidx - The zone index of the highest zone to return
 *
 * This iterator iterates though all zones at or below a given zone index.
 */
#define for_each_zone_zonelist(zone, z, zlist, highidx) \
	for_each_zone_zonelist_nodemask(zone, z, zlist, highidx, NULL)

#ifdef CONFIG_SPARSEMEM
#include <asm/sparsemem.h>
#endif

#if !defined(CONFIG_HAVE_ARCH_EARLY_PFN_TO_NID) && \
	!defined(CONFIG_HAVE_MEMBLOCK_NODE_MAP)
static inline unsigned long early_pfn_to_nid(unsigned long pfn)
{
	return 0;
}
#endif

#ifdef CONFIG_FLATMEM
#define pfn_to_nid(pfn)		(0)
#endif

#ifdef CONFIG_SPARSEMEM

/*
 * SECTION_SHIFT    		#bits space required to store a section #
 *
 * PA_SECTION_SHIFT		physical address to/from section number
 * PFN_SECTION_SHIFT		pfn to/from section number
 */
#define SECTIONS_SHIFT		(MAX_PHYSMEM_BITS - SECTION_SIZE_BITS)

#define PA_SECTION_SHIFT	(SECTION_SIZE_BITS)
#define PFN_SECTION_SHIFT	(SECTION_SIZE_BITS - PAGE_SHIFT)

#define NR_MEM_SECTIONS		(1UL << SECTIONS_SHIFT)

#define PAGES_PER_SECTION       (1UL << PFN_SECTION_SHIFT)
#define PAGE_SECTION_MASK	(~(PAGES_PER_SECTION-1))

#define SECTION_BLOCKFLAGS_BITS \
	((1UL << (PFN_SECTION_SHIFT - pageblock_order)) * NR_PAGEBLOCK_BITS)

#if (MAX_ORDER - 1 + PAGE_SHIFT) > SECTION_SIZE_BITS
#error Allocator MAX_ORDER exceeds SECTION_SIZE
#endif

#define pfn_to_section_nr(pfn) ((pfn) >> PFN_SECTION_SHIFT)
#define section_nr_to_pfn(sec) ((sec) << PFN_SECTION_SHIFT)

#define SECTION_ALIGN_UP(pfn)	(((pfn) + PAGES_PER_SECTION - 1) & PAGE_SECTION_MASK)
#define SECTION_ALIGN_DOWN(pfn)	((pfn) & PAGE_SECTION_MASK)

struct page;
struct page_cgroup;
struct mem_section {
	/*
	 * This is, logically, a pointer to an array of struct
	 * pages.  However, it is stored with some other magic.
	 * (see sparse.c::sparse_init_one_section())
	 *
	 * Additionally during early boot we encode node id of
	 * the location of the section here to guide allocation.
	 * (see sparse.c::memory_present())
	 *
	 * Making it a UL at least makes someone do a cast
	 * before using it wrong.
	 */
	unsigned long section_mem_map;

	/* See declaration of similar field in struct zone */
	unsigned long *pageblock_flags;
#ifdef CONFIG_MEMCG
	/*
	 * If !SPARSEMEM, pgdat doesn't have page_cgroup pointer. We use
	 * section. (see memcontrol.h/page_cgroup.h about this.)
	 */
	struct page_cgroup *page_cgroup;
	unsigned long pad;
#endif
};

#ifdef CONFIG_SPARSEMEM_EXTREME
#define SECTIONS_PER_ROOT       (PAGE_SIZE / sizeof (struct mem_section))
#else
#define SECTIONS_PER_ROOT	1
#endif

#define SECTION_NR_TO_ROOT(sec)	((sec) / SECTIONS_PER_ROOT)
#define NR_SECTION_ROOTS	DIV_ROUND_UP(NR_MEM_SECTIONS, SECTIONS_PER_ROOT)
#define SECTION_ROOT_MASK	(SECTIONS_PER_ROOT - 1)

#ifdef CONFIG_SPARSEMEM_EXTREME
extern struct mem_section *mem_section[NR_SECTION_ROOTS];
#else
extern struct mem_section mem_section[NR_SECTION_ROOTS][SECTIONS_PER_ROOT];
#endif

static inline struct mem_section *__nr_to_section(unsigned long nr)
{
	if (!mem_section[SECTION_NR_TO_ROOT(nr)])
		return NULL;
	return &mem_section[SECTION_NR_TO_ROOT(nr)][nr & SECTION_ROOT_MASK];
}
extern int __section_nr(struct mem_section* ms);
extern unsigned long usemap_size(void);

/*
 * We use the lower bits of the mem_map pointer to store
 * a little bit of information.  There should be at least
 * 3 bits here due to 32-bit alignment.
 */
#define	SECTION_MARKED_PRESENT	(1UL<<0)
#define SECTION_HAS_MEM_MAP	(1UL<<1)
#define SECTION_MAP_LAST_BIT	(1UL<<2)
#define SECTION_MAP_MASK	(~(SECTION_MAP_LAST_BIT-1))
#define SECTION_NID_SHIFT	2

static inline struct page *__section_mem_map_addr(struct mem_section *section)
{
	unsigned long map = section->section_mem_map;
	map &= SECTION_MAP_MASK;
	return (struct page *)map;
}

static inline int present_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_MARKED_PRESENT));
}

static inline int present_section_nr(unsigned long nr)
{
	return present_section(__nr_to_section(nr));
}

static inline int valid_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_HAS_MEM_MAP));
}

static inline int valid_section_nr(unsigned long nr)
{
	return valid_section(__nr_to_section(nr));
}

static inline struct mem_section *__pfn_to_section(unsigned long pfn)
{
	return __nr_to_section(pfn_to_section_nr(pfn));
}

#ifndef CONFIG_HAVE_ARCH_PFN_VALID
static inline int pfn_valid(unsigned long pfn)
{
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;
	return valid_section(__nr_to_section(pfn_to_section_nr(pfn)));
}
#endif

static inline int pfn_present(unsigned long pfn)
{
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;
	return present_section(__nr_to_section(pfn_to_section_nr(pfn)));
}

/*
 * These are _only_ used during initialisation, therefore they
 * can use __initdata ...  They could have names to indicate
 * this restriction.
 */
#ifdef CONFIG_NUMA
#define pfn_to_nid(pfn)							\
({									\
	unsigned long __pfn_to_nid_pfn = (pfn);				\
	page_to_nid(pfn_to_page(__pfn_to_nid_pfn));			\
})
#else
#define pfn_to_nid(pfn)		(0)
#endif

#define early_pfn_valid(pfn)	pfn_valid(pfn)
void sparse_init(void);
#else
#define sparse_init()	do {} while (0)
#define sparse_index_init(_sec, _nid)  do {} while (0)
#endif /* CONFIG_SPARSEMEM */

#ifdef CONFIG_NODES_SPAN_OTHER_NODES
bool early_pfn_in_nid(unsigned long pfn, int nid);
#else
#define early_pfn_in_nid(pfn, nid)	(1)
#endif

#ifndef early_pfn_valid
#define early_pfn_valid(pfn)	(1)
#endif

void memory_present(int nid, unsigned long start, unsigned long end);
unsigned long __init node_memmap_size_bytes(int, unsigned long, unsigned long);

/*
 * If it is possible to have holes within a MAX_ORDER_NR_PAGES, then we
 * need to check pfn validility within that MAX_ORDER_NR_PAGES block.
 * pfn_valid_within() should be used in this case; we optimise this away
 * when we have no holes within a MAX_ORDER_NR_PAGES block.
 */
#ifdef CONFIG_HOLES_IN_ZONE
#define pfn_valid_within(pfn) pfn_valid(pfn)
#else
#define pfn_valid_within(pfn) (1)
#endif

#ifdef CONFIG_ARCH_HAS_HOLES_MEMORYMODEL
/*
 * pfn_valid() is meant to be able to tell if a given PFN has valid memmap
 * associated with it or not. In FLATMEM, it is expected that holes always
 * have valid memmap as long as there is valid PFNs either side of the hole.
 * In SPARSEMEM, it is assumed that a valid section has a memmap for the
 * entire section.
 *
 * However, an ARM, and maybe other embedded architectures in the future
 * free memmap backing holes to save memory on the assumption the memmap is
 * never used. The page_zone linkages are then broken even though pfn_valid()
 * returns true. A walker of the full memmap must then do this additional
 * check to ensure the memmap they are looking at is sane by making sure
 * the zone and PFN linkages are still valid. This is expensive, but walkers
 * of the full memmap are extremely rare.
 */
int memmap_valid_within(unsigned long pfn,
					struct page *page, struct zone *zone);
#else
static inline int memmap_valid_within(unsigned long pfn,
					struct page *page, struct zone *zone)
{
	return 1;
}
#endif /* CONFIG_ARCH_HAS_HOLES_MEMORYMODEL */

#endif /* !__GENERATING_BOUNDS.H */
#endif /* !__ASSEMBLY__ */
#endif /* _LINUX_MMZONE_H */
