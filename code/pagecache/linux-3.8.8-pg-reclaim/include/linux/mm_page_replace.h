#include <linux/mmzone.h>
#include <linux/mm.h>
#include <linux/pagevec.h>
#include <linux/seq_file.h>

/*
struct scan_control {
	unsigned long nr_to_scan;

	unsigned long nr_scanned;

	unsigned long nr_reclaimed;

	unsigned long nr_writeout;	// page against which writeout was started 

	unsigned long nr_mapped;	// From page_state

	unsigned int priority;

	gfp_t gfp_mask;

	int may_writepage;

	int may_swap;

	int swap_cluster_max;
};

#define lru_to_page(_head) (list_entry((_head)->prev, struct page, lru))

#ifdef ARCH_HAS_PREFETCH
#define prefetch_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetch(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetch_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

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
*/

extern void page_replace_init(void);
extern void page_replace_init_zone(struct zone *);
/* void page_replace_hint_active(struct page *); */
/* void page_replace_hint_use_once(struct page *); */
extern void  page_replace_add(struct page *);
/* void __page_replace_add(struct zone *, struct page *); */
/* void page_replace_add_drain(void); */
extern void __page_replace_add_drain(unsigned int);
extern int page_replace_add_drain_all(void);
extern void __pagevec_page_replace_add(struct pagevec *);

typedef enum {
	RECLAIM_KEEP,
	RECLAIM_ACTIVATE,
	RECLAIM_REFERENCED,
	RECLAIM_OK,
} reclaim_t;

/* reclaim_t page_replace_reclaimable(struct page *); */
/* int page_replace_activate(struct page *page); */
//extern void page_replace_reinsert(struct list_head *);
//extern void page_replace_shrink(struct zone *, struct scan_control *);
/* void page_replace_mark_accessed(struct page *); */
/* void page_replace_remove(struct zone *, struct page *); */
/* void __page_replace_rotate_reclaimable(struct zone *, struct page *); */
/* void page_replace_copy_state(struct page *, struct page *); */
/* void page_replace_clear_state(struct page *); */
/* int page_replace_is_active(struct page *); */
/* void page_replace_remember(struct zone *, struct page*); */
/* void page_replace_forget(struct address_space *, unsigned long); */
extern void page_replace_show(struct zone *);
extern void page_replace_zoneinfo(struct zone *, struct seq_file *);
extern void __page_replace_counts(unsigned long *, unsigned long *,
				  unsigned long *, struct pglist_data *);
/* unsigned long __page_replace_nr_pages(struct zone *); */

/*
#ifdef CONFIG_MIGRATION
extern int page_replace_isolate(struct page *p);
#else
static inline int page_replace_isolate(struct page *p) { return -ENOSYS; }
#endif
*/

/*
#if defined CONFIG_MM_POLICY_USEONCE
#include <linux/mm_use_once_policy.h>
#elif defined CONFIG_MM_POLICY_CLOCKPRO
#include <linux/mm_clockpro_policy.h>
#elif defined CONFIG_MM_POLICY_CART || defined CONFIG_MM_POLICY_CART_R
#include <linux/mm_cart_policy.h>
#elif defined CONFIG_MM_POLICY_RANDOM
#include <linux/mm_random_policy.h>
#else
#error no mm policy
#endif
*/

/*
static inline void pagevec_page_replace_add(struct pagevec *pvec)
{
	if (pagevec_count(pvec))
		__pagevec_page_replace_add(pvec);
}

static inline void page_replace_add_drain(void)
{
	__page_replace_add_drain(get_cpu());
	put_cpu();
}
*/
#if ! defined MM_POLICY_HAS_SHRINKER
/* unsigned long __page_replace_nr_scan(struct zone *); */
//void page_replace_candidates(struct zone *, int, struct list_head *);
//void page_replace_reinsert_zone(struct zone *, struct list_head *, int);
#endif
