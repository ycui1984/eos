#include <linux/statedef.h>
#include <linux/fingerprint.h>
#include <linux/bio.h>
#include <linux/genhd.h>
#include <linux/sched.h>
#include <linux/export.h>

#ifdef SUBSYS_DISK

int IOCNT_THD = 10;
EXPORT_SYMBOL(IOCNT_THD);

static void update_total_size (struct fp_snapshot *ss, unsigned long size)
{
	ss->total_size += size; 
}

static void update_total_tt (struct fp_snapshot *ss, unsigned long start)
{
	unsigned long tmp_tt;
	
	if (0 == ss->start) {
		ss->start = start;
		return;	
	}
	
	if (start >= ss->start) {
		tmp_tt = start - ss->start;
		ss->total_tt += tmp_tt;
		ss->start = start;	
	} else {
		printk("timing BUG\n");
		printk("start=%lu, last start=%lu\n", start, ss->start);
	}
}

static void update_total_dist(struct fp_snapshot *ss, unsigned long head_pos)
{
	long tmp_dist;
	
	if (0 == ss->head_pos) {
		ss->head_pos = head_pos;
		return;
	}
	
	tmp_dist = ss->head_pos - head_pos;
	if (tmp_dist < 0) 
		tmp_dist = -tmp_dist;

	ss->total_dist += tmp_dist;
	ss->head_pos = head_pos;	
}

static void update_total_ids(struct fp_snapshot *ss, int id, int tag, int *iocnt)
{
	int i, found = 0;
	
	if (0 == tag) return;
	(*iocnt)++;	

	if (*iocnt < IOCNT_THD) return;

	for (i = 0; i <= ss->recorder.guard; i++) 
		if (id == ss->recorder.id_array[i]) {
			found = 1; 
			break;
		}
	
	if (0 == found && ss->recorder.guard < MAXIDS - 1) {
		ss->recorder.guard ++;
		ss->recorder.id_array[ss->recorder.guard] = id;
	}
}

static inline unsigned long read_tsc(void)
{
	unsigned int low, high;
   	__asm__ __volatile__("rdtsc" : "=a" (low),"=d" (high));
   	return (unsigned long)high << 32 | low;
}

void update_fp_snapshot(struct bio *bio)
{
	struct fp_snapshot *ss = &(bio->bi_bdev->bd_disk->fp_ss);
#ifdef AUTO_HRTIME	
	struct timespec ts; 
#endif
	
	if (READ == bio_data_dir(bio)) 
		ss->reads ++;
	else
		ss->writes ++;

	update_total_size(ss, bio_sectors(bio));	
#ifdef AUTO_HRTIME
	getnstimeofday(&ts);
	update_total_tt(ss, timespec_to_ns(&ts));
#else
	update_total_tt(ss, read_tsc());
#endif
	update_total_dist(ss, bio->bi_sector);
	update_total_ids(ss, current->pid, current->tag, &(current->iocnt));
}
#endif
