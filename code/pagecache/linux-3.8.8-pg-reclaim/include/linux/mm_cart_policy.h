#include <linux/rmap.h>
#include <linux/page-flags.h>

//#define PG_t1		PG_reclaim1
//#define PG_longterm	PG_reclaim2
//#define PG_new		PG_reclaim3
#define PG_t1		0
#define PG_longterm	1
#define PG_new		2

#define CART_PageT1(page)		test_bit(PG_t1, &(page)->state)
#define CART_SetPageT1(page)		set_bit(PG_t1, &(page)->state)
#define CART_ClearPageT1(page)	clear_bit(PG_t1, &(page)->state)
#define CART_TestClearPageT1(page)	test_and_clear_bit(PG_t1, &(page)->state)
#define CART_TestSetPageT1(page)	test_and_set_bit(PG_t1, &(page)->state)

#define CART_PageLongTerm(page)	test_bit(PG_longterm, &(page)->state)
#define CART_SetPageLongTerm(page)	set_bit(PG_longterm, &(page)->state)
#define CART_TestSetPageLongTerm(page) test_and_set_bit(PG_longterm, &(page)->state)
#define CART_ClearPageLongTerm(page)	clear_bit(PG_longterm, &(page)->state)
#define CART_TestClearPageLongTerm(page) test_and_clear_bit(PG_longterm, &(page)->state)

#define CART_PageNew(page)		test_bit(PG_new, &(page)->state)
#define CART_SetPageNew(page)	set_bit(PG_new, &(page)->state)
#define CART_TestSetPageNew(page)	test_and_set_bit(PG_new, &(page)->state)
#define CART_ClearPageNew(page)	clear_bit(PG_new, &(page)->state)
#define CART_TestClearPageNew(page)	test_and_clear_bit(PG_new, &(page)->state)

static inline void page_replace_hint_active(struct page *page)
{
}

static inline void page_replace_hint_use_once(struct page *page)
{
	if (PageLRU(page))
		BUG();
	CART_SetPageNew(page);
}

//extern void __page_replace_add(struct zone *, struct page *);
extern void __page_replace_add(struct zone *, struct page *);

static inline void page_replace_copy_state(struct page *dpage, struct page *spage)
{
	if (CART_PageT1(spage))
		CART_SetPageT1(dpage);
	if (CART_PageLongTerm(spage))
		CART_SetPageLongTerm(dpage);
	if (CART_PageNew(spage))
		CART_SetPageNew(dpage);
}

static inline void page_replace_clear_state(struct page *page)
{
	if (CART_PageT1(page))
		CART_ClearPageT1(page);
	if (CART_PageLongTerm(page))
		CART_ClearPageLongTerm(page);
	if (CART_PageNew(page))
		CART_ClearPageNew(page);
}

static inline int page_replace_is_active(struct page *page)
{
	return CART_PageLongTerm(page);
}


/*
static inline int page_replace_reclaimable(struct page *page)
{
	if (page_referenced(page, 1, 0))
		return RECLAIM_ACTIVATE;

	if (CART_PageNew(page))
		CART_ClearPageNew(page);

	if ((CART_PageT1(page) && CART_PageLongTerm(page)) ||
	    (!CART_PageT1(page) && !CART_PageLongTerm(page)))
		return RECLAIM_KEEP;

	return RECLAIM_OK;
}
*/

static inline int page_replace_activate(struct page *page)
{
	/* just set PG_referenced, handle the rest in
	 * page_replace_reinsert()
	 */
	if (!CART_TestClearPageNew(page)) {
		SetPageReferenced(page);
		return 1;
	}

	return 0;
}

//extern void __page_replace_rotate_reclaimable(struct zone *, struct page *);

static inline void page_replace_mark_accessed(struct page *page)
{
	SetPageReferenced(page);
}

#define MM_POLICY_HAS_NONRESIDENT

extern void page_replace_remember(struct zone *, struct page *);
extern void page_replace_forget(struct address_space *, unsigned long);
