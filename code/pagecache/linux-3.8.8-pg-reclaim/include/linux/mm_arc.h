#include <asm/bitops.h>

#include <linux/rmap.h>
#include <linux/page-flags.h>

#define ARC_RECLAIMED_MRU	0
#define ARC_RECLAIMED_MFU	1
#define ARC_SATURATED		2

#define ZoneReclaimedMRU(z)	test_bit(ARC_RECLAIMED_MRU, &((z)->flags))
#define ZoneReclaimedMFU(z)	test_bit(ARC_RECLAIMED_MFU, &((z)->flags))
#define SetZoneReclaimedMRU(z)	__set_bit(ARC_RECLAIMED_MRU, &((z)->flags))
#define SetZoneReclaimedMFU(z)	__set_bit(ARC_RECLAIMED_MFU, &((z)->flags))
#define ClearZoneReclaimedMRU(z)	__clear_bit(ARC_RECLAIMED_MRU, &((z)->flags))
#define ClearZoneReclaimedMFU(z)	__clear_bit(ARC_RECLAIMED_MFU, &((z)->flags))

#define ZoneSaturated(z)	test_bit(ARC_SATURATED, &((z)->flags))
#define SetZoneSaturated(z)	__set_bit(ARC_SATURATED, &((z)->flags))
#define TestClearZoneSaturated(z)  __test_and_clear_bit(ARC_SATURATED, &((z)->flags))

#define PG_mru		0
#define PG_mfu		1
#define PG_new		2

#define ARC_PageMRU(page)		test_bit(PG_mru, &(page)->state)
#define ARC_SetPageMRU(page)		set_bit(PG_mru, &(page)->state)
#define ARC_ClearPageMRU(page)	clear_bit(PG_mru, &(page)->state)
#define ARC_TestClearPageMRU(page)	test_and_clear_bit(PG_mru, &(page)->state)
#define ARC_TestSetPageMRU(page)	test_and_set_bit(PG_mru, &(page)->state)

#define ARC_PageMFU(page)		test_bit(PG_mfu, &(page)->state)
#define ARC_SetPageMFU(page)		set_bit(PG_mfu, &(page)->state)
#define ARC_ClearPageMFU(page)	clear_bit(PG_mfu, &(page)->state)
#define ARC_TestClearPageMFU(page)	test_and_clear_bit(PG_mfu, &(page)->state)
#define ARC_TestSetPageMFU(page)	test_and_set_bit(PG_mfu, &(page)->state)

#define ARC_PageNew(page)		test_bit(PG_new, &(page)->state)
#define ARC_SetPageNew(page)	set_bit(PG_new, &(page)->state)
#define ARC_TestSetPageNew(page)	test_and_set_bit(PG_new, &(page)->state)
#define ARC_ClearPageNew(page)	clear_bit(PG_new, &(page)->state)
#define ARC_TestClearPageNew(page)	test_and_clear_bit(PG_new, &(page)->state)

static inline void page_replace_hint_use_once(struct page *page)
{
	if (PageLRU(page))
		BUG();
	ARC_SetPageNew(page);
}

static inline int page_replace_is_active(struct page *page)
{
	return ARC_PageMFU(page);
}

static inline int page_replace_activate(struct page *page)
{
	/* just set PG_referenced, handle the rest in
	 * page_replace_reinsert()
	 */
	if (!ARC_TestClearPageNew(page)) {
		SetPageReferenced(page);
		return 1;
	}

	return 0;
}

static inline void page_replace_clear_state(struct page *page)
{
	if (ARC_PageMRU(page))
		ARC_ClearPageMRU(page);
	if (ARC_PageMFU(page))
		ARC_ClearPageMFU(page);
	if (ARC_PageNew(page))
		ARC_ClearPageNew(page);
}

static inline void page_replace_mark_accessed(struct page *page)
{
	SetPageReferenced(page);
}

extern void page_replace_remember_arc(struct zone *, struct page *);
extern void page_replace_forget_arc(struct address_space *, unsigned long);

