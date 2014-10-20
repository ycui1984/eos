/*
 * mm/nonresident-cart.c
 * (C) 2004,2005 Red Hat, Inc
 * Written by Rik van Riel <riel@redhat.com>
 * Released under the GPL, see the file COPYING for details.
 * Adapted by Peter Zijlstra <a.p.zijlstra@chello.nl> for use by ARC
 * like algorithms.
 *
 * Keeps track of whether a non-resident page was recently evicted
 * and should be immediately promoted to the active list. This also
 * helps automatically tune the inactive target.
 *
 * The pageout code stores a recently evicted page in this cache
 * by calling nonresident_put(mapping/mm, index/vaddr)
 * and can look it up in the cache by calling nonresident_find()
 * with the same arguments.
 *
 * Note that there is no way to invalidate pages after eg. truncate
 * or exit, we let the pages fall out of the non-resident set through
 * normal replacement.
 *
 *
 * Modified to work with ARC like algorithms who:
 *  - need to balance two FIFOs; |b1| + |b2| = c,
 *
 * The bucket contains four single linked cyclic lists (CLOCKS) and each
 * clock has a tail hand. By selecting a victim clock upon insertion it
 * is possible to balance them.
 *
 * The first two lists are used for B1/B2 and a third for a free slot list.
 * The fourth list is unused.
 *
 * The slot looks like this:
 * struct slot_t {
 *         u32 cookie : 24; // LSB
 *         u32 index  :  6;
 *         u32 listid :  2;
 * };
 *
 * The bucket is guarded by a spinlock.
 */
#include <linux/swap.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/cache.h>
#include <linux/spinlock.h>
#include <linux/bootmem.h>
#include <linux/hash.h>
#include <linux/prefetch.h>
#include <linux/kernel.h>
#include <linux/nonresident.h>

#define TARGET_SLOTS	64
#define NR_CACHELINES  (TARGET_SLOTS*sizeof(u32) / L1_CACHE_BYTES)
#define NR_SLOTS	(((NR_CACHELINES * L1_CACHE_BYTES) - sizeof(spinlock_t) - 4*sizeof(u8)) / sizeof(u32))
#if 0
#if NR_SLOTS < (TARGET_SLOTS / 2)
#warning very small slot size
#if NR_SLOTS <= 0
#error no room for slots left
#endif
#endif
#endif

#define BUILD_MASK(bits, shift) (((1 << (bits)) - 1) << (shift))

#define LISTID_BITS		2
#define LISTID_SHIFT		(sizeof(u32)*8 - LISTID_BITS)
#define LISTID_MASK		BUILD_MASK(LISTID_BITS, LISTID_SHIFT)

#define SET_LISTID(x, flg)	((x) = ((x) & ~LISTID_MASK) | ((flg) << LISTID_SHIFT))
#define GET_LISTID(x)		(((x) & LISTID_MASK) >> LISTID_SHIFT)

#define INDEX_BITS		6  /* ceil(log2(NR_SLOTS)) */
#define INDEX_SHIFT		(LISTID_SHIFT - INDEX_BITS)
#define INDEX_MASK		BUILD_MASK(INDEX_BITS, INDEX_SHIFT)

#define SET_INDEX(x, idx)	((x) = ((x) & ~INDEX_MASK) | ((idx) << INDEX_SHIFT))
#define GET_INDEX(x)		(((x) & INDEX_MASK) >> INDEX_SHIFT)

#define COOKIE_MASK		BUILD_MASK(sizeof(u32)*8 - LISTID_BITS - INDEX_BITS, 0)


struct nr_bucket
{
	spinlock_t lock;
	u8 hand[4];
	u32 slot[NR_SLOTS];
} ____cacheline_aligned;

/* The non-resident page hash table. */
static struct nr_bucket * nonres_table;
static unsigned int nonres_shift;
static unsigned int nonres_mask;

static int RECLAIM_POLICY;

void nonresident_set_policy(int policy)
{
	RECLAIM_POLICY = policy;
}

int nonresident_get_policy()
{
	return RECLAIM_POLICY;
}
/* hash the address into a bucket */
static struct nr_bucket * nr_hash(void * mapping, unsigned long index)
{
	unsigned long bucket;
	unsigned long hash;

	
	hash = (unsigned long)mapping + 37 * index;
	bucket = hash_long(hash, nonres_shift);

	return nonres_table + bucket;
}

unsigned int nonresident_total()
{
	return (1 << nonres_shift) * NR_SLOTS;
}

/* hash the address and inode into a cookie */
static u32 nr_cookie(struct address_space * mapping, unsigned long index)
{
	unsigned long hash;

	hash = 37 * (unsigned long)mapping + index;

	if (mapping && mapping->host)
		hash = 37 * hash + mapping->host->i_ino;

	return hash_long(hash, sizeof(u32)*8 - LISTID_BITS - INDEX_BITS);
}

DEFINE_PER_CPU(unsigned long[4], nonres_count);

/*
 * remove current (b from 'abc'):
 *
 *    initial        swap(2,3)
 *
 *   1: -> [2],a     1: -> [2],a
 * * 2: -> [3],b     2: -> [1],c
 *   3: -> [1],c   * 3: -> [3],b
 *
 *   3 is now free for use.
 *
 * @nr_bucket: bucket to operate in
 * @listid: list that the deletee belongs to
 * @pos: slot position of deletee
 * @slot: possible pointer to slot
 *
 * returns pointer to removed slot, NULL when list empty.
 */
static u32 * __nonresident_del(struct nr_bucket *nr_bucket, int listid, 
		u8 pos, u32 *slot)
{
	int next_pos;
	u32 *next;

	if (slot == NULL) {
		slot = &nr_bucket->slot[pos];
		if (GET_LISTID(*slot) != listid)
			return NULL;
	}

	--__get_cpu_var(nonres_count[listid]);

	next_pos = GET_INDEX(*slot);
	if (pos == next_pos) {
		next = slot;
		goto out;
	}

	next = &nr_bucket->slot[next_pos];
	*next = xchg(slot, *next);

	if (next_pos == nr_bucket->hand[listid])
		nr_bucket->hand[listid] = pos;
out:
	BUG_ON(GET_INDEX(*next) != next_pos);
	return next;
}

static inline u32 * __nonresident_pop(struct nr_bucket *nr_bucket, int listid)
{
	return __nonresident_del(nr_bucket, listid, nr_bucket->hand[listid], NULL);
}

/*
 * insert before (d before b in 'abc')
 *
 *    initial          set 4         swap(2,4)
 *
 *   1: -> [2],a     1: -> [2],a    1: -> [2],a
 * * 2: -> [3],b     2: -> [3],b    2: -> [4],d
 *   3: -> [1],c     3: -> [1],c    3: -> [1],c
 *   4: -> [4],nil   4: -> [4],d  * 4: -> [3],b
 *
 *   leaving us with 'adbc'.
 *
 * @nr_bucket: bucket to operator in
 * @listid: list to insert into
 * @pos: position to insert before
 * @slot: slot to insert
 */
static void __nonresident_insert(struct nr_bucket *nr_bucket, int listid,
											u8 *pos, u32 *slot)
{
	u32 *head;

	SET_LISTID(*slot, listid);

	head = &nr_bucket->slot[*pos];

	*pos = GET_INDEX(*slot);
	if (GET_LISTID(*head) == listid)
		*slot = xchg(head, *slot);

	++__get_cpu_var(nonres_count[listid]);
}

static inline void __nonresident_push(struct nr_bucket *nr_bucket, int listid, 
												u32 *slot)
{
	__nonresident_insert(nr_bucket, listid, &nr_bucket->hand[listid], slot);
}

/*
 * Remembers a page by putting a hash-cookie on the @listid list.
 *
 * @mapping: page_mapping()
 * @index: page_index()
 * @listid: list to put the page on (NR_b1, NR_b2 and NR_free).
 * @listid_evict: list to get a free page from when NR_free is empty.
 *
 * returns the list an empty page was taken from.
 */
int nonresident_put(struct address_space * mapping, unsigned long index,
				int listid, int listid_evict)
{
	struct nr_bucket *nr_bucket;
	u32 cookie;
	unsigned long flags;
	u32 *slot;
	int evict = NR_free;

	prefetch(mapping->host);
	nr_bucket = nr_hash(mapping, index);

	spin_lock_prefetch(nr_bucket); // prefetchw_range(nr_bucket, NR_CACHELINES);
	cookie = nr_cookie(mapping, index);
	
	spin_lock_irqsave(&nr_bucket->lock, flags);
	
	slot = __nonresident_pop(nr_bucket, evict);
	if (!slot) {
		evict = listid_evict;
		printk(KERN_ERR "nonresident_put: get slot from %d, nonresident_total %d\n", evict, nonresident_total());
		slot = __nonresident_pop(nr_bucket, evict);
		if (!slot) {
			printk(KERN_ERR "Failed: try to get slot from %d, nonresident_total %d\n", evict, nonresident_total());
			evict ^= 1;
			slot = __nonresident_pop(nr_bucket, evict);
		}
	}

//	printk(KERN_ERR "nonresident_put: slot %x, %lu, %d, %d\n", *slot, index, listid, listid_evict);
	
	BUG_ON(!slot);
	SET_INDEX(cookie, GET_INDEX(*slot));
	cookie = xchg(slot, cookie);
	__nonresident_push(nr_bucket, listid, slot);
	spin_unlock_irqrestore(&nr_bucket->lock, flags);

	return evict;
}

/*
 * Searches a page on the first two lists, and places it on the free list.
 *
 * @mapping: page_mapping()
 * @index: page_index()
 *
 * returns listid of the list the item was found on with NR_found set if found.
 */
int nonresident_get(struct address_space * mapping, unsigned long index)
{
	struct nr_bucket * nr_bucket;
	u32 wanted;
	int j;
	u8 i;
	unsigned long flags;
	int ret = 0;

	if (mapping)
		prefetch(mapping->host);
	nr_bucket = nr_hash(mapping, index);

	spin_lock_prefetch(nr_bucket); // prefetch_range(nr_bucket, NR_CACHELINES);
	wanted = nr_cookie(mapping, index) & COOKIE_MASK;

	spin_lock_irqsave(&nr_bucket->lock, flags);
	for (i = 0; i < 2; ++i) {
		j = nr_bucket->hand[i];
		do {
			u32 *slot = &nr_bucket->slot[j];
			if (GET_LISTID(*slot) != i)
				break;

			if ((*slot & COOKIE_MASK) == wanted) {
				slot = __nonresident_del(nr_bucket, i, j, slot);
				__nonresident_push(nr_bucket, NR_free, slot);
				ret = i | NR_found;
				goto out;
			}

			j = GET_INDEX(*slot);
		} while (j != nr_bucket->hand[i]);
	}
out:
	spin_unlock_irqrestore(&nr_bucket->lock, flags);

	return ret;
}

void nonres_forget(struct address_space *mapping, unsigned long index)
{
	int policy = nonresident_get_policy();

	pgreclaim_policies[policy]->nonres_forget(mapping, index);
}

/*
 * For interactive workloads, we remember about as many non-resident pages
 * as we have actual memory pages.  For server workloads with large inter-
 * reference distances we could benefit from remembering more.
 */
static __initdata unsigned long nonresident_factor = 1;

void __init nonresident_init(void)
{
	int target;
	int i, j;

	/*
	 * Calculate the non-resident hash bucket target. Use a power of
	 * two for the division because alloc_large_system_hash rounds up.
	 */
	target = nr_all_pages * nonresident_factor;
	target /= (sizeof(struct nr_bucket) / sizeof(u32));
//	target /= NR_SLOTS;

	nonres_table = alloc_large_system_hash("Non-resident page tracking",
					sizeof(struct nr_bucket),
					target,
					0,
//					HASH_EARLY | HASH_HIGHMEM,
//					HASH_EARLY,
					0,
					&nonres_shift,
					&nonres_mask,
					0, 0);

	printk(KERN_ERR "init_nonresident_init: hash bucket target %d, nr_all_pages: %ld, total %d\n", 
			target, nr_all_pages, nonresident_total());

	for (i = 0; i < (1 << nonres_shift); i++) {
		spin_lock_init(&nonres_table[i].lock);
		for (j = 0; j < 4; ++j)
			nonres_table[i].hand[j] = 0;

		for (j = 0; j < NR_SLOTS; ++j) {
			nonres_table[i].slot[j] = 0;
			SET_LISTID(nonres_table[i].slot[j], NR_free);
			if (j < NR_SLOTS - 1)
				SET_INDEX(nonres_table[i].slot[j], j+1);
			else /* j == NR_SLOTS - 1 */
				SET_INDEX(nonres_table[i].slot[j], 0);
		}
	}

	for_each_cpu(i, cpu_possible_mask) {
		for (j=0; j<4; ++j)
			per_cpu(nonres_count[j], i) = 0;
	}
}

static int __init set_nonresident_factor(char * str)
{
	if (!str)
		return 0;
	nonresident_factor = simple_strtoul(str, &str, 0);
	return 1;
}

__setup("nonresident_factor=", set_nonresident_factor);
