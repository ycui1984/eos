#ifndef __FINGERPRINT_H
#define __FINGERPRINT_H

#define MAXIDS		50

#ifdef SUBSYS_DISK
#include <linux/blk_types.h>

struct ID_RED {
	int id_array[MAXIDS];
	int guard;
};
 
struct fp_snapshot {
	unsigned long reads;
	unsigned long writes;
	unsigned long start;
	unsigned long head_pos;
	unsigned long total_size;	
	unsigned long total_tt;
	unsigned long total_dist;	
	struct ID_RED recorder;
};

void update_fp_snapshot(struct bio *bio);
#elif defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)
struct fp_snapshot {
	unsigned long lock_time;
	unsigned long lock_cnt;
};
#endif
#endif
