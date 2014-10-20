#include <asm/bitops.h>

/*
struct page_replace_data {
	struct list_head        list_T1;
	struct list_head        list_T2;
	unsigned long		nr_scan;
	unsigned long		nr_T1;
	unsigned long		nr_T2;
	unsigned long           nr_shortterm;
	unsigned long           nr_p;
	unsigned long		flags;
};
*/

#define CART_RECLAIMED_T1	0
#define CART_SATURATED		1

#define ZoneReclaimedT1(z)	test_bit(CART_RECLAIMED_T1, &((z)->flags))
#define SetZoneReclaimedT1(z)	__set_bit(CART_RECLAIMED_T1, &((z)->flags))
#define ClearZoneReclaimedT1(z)	__clear_bit(CART_RECLAIMED_T1, &((z)->flags))

#define ZoneSaturated(z)	test_bit(CART_SATURATED, &((z)->flags))
#define SetZoneSaturated(z)	__set_bit(CART_SATURATED, &((z)->flags))
#define TestClearZoneSaturated(z)  __test_and_clear_bit(CART_SATURATED, &((z)->flags))
