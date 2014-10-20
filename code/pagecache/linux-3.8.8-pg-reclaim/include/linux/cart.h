#ifndef __CART_H__
#define __CART_H__

/* Short and Long term utility as used by CART */
#define SHORT_TERM	0
#define LONG_TERM	1

#define T1 		0x1
#define T2		0x2

#define B1		0x4
#define B2		0x8

/* The struct used to represent non resident pages */
struct non_res_list_node {
	unsigned long mapping;
	unsigned long offset;
	struct list_head list;
};

/* Functions to manipulate the "utility" of a page */
int cart_init();
void set_short_term(struct page *page);
void set_long_term(struct page *page);

/* List manipulation functions */
void add_to_resident_list(struct page *page, unsigned int list);
void add_to_non_resident_list (struct page *page, unsigned int list);
unsigned int find_in_resident_list(struct page *page);
unsigned int find_in_non_resident_list(struct page *page);

void add_to_t1(struct page *page);
void add_to_t2 (struct page *page);
void add_to_t1_tail(struct zone *, struct page *);
void add_to_t2_tail(struct zone *, struct page *);
unsigned int is_in_t1(unsigned int flag);
unsigned int is_in_t2(unsigned int flag);
unsigned int is_in_b1(unsigned int flag);
unsigned int is_in_b2(unsigned int flag);
void update_cart_params(struct page *page);
struct page *replace (struct zone *);

#endif
