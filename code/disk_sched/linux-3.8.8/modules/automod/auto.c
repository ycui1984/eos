#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/random.h>
#include <asm/uaccess.h>
#include <linux/genhd.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/netdevice.h>
#include <linux/statedef.h>
#include <linux/fingerprint.h>
#include <net/tcp.h>
#include <linux/inet_diag.h>
#include <linux/skbuff.h>
#include <linux/mm.h>

#define MAXLEN			256
#define CONFIG_LIFE_TIME	5000
#define MAXTYPE			20
#define MAXNUM			10
#define RGNUM			3
#define MINVAL			0
#define MAXVAL			1
#define STEP			2
#define DOM_ID			1
#define DOM_SUBSYS		2
#define DOM_PROP		3
#define DOM_LOC			4
#define DOM_TYPE		5
#define DOM_DEF			6
#define MAX_BUF_SIZE		256
#define ON			1
#define OFF			0
#define TOTAL_DOM		7

#ifdef SUBSYS_DISK
#define DISKTHP
#endif

#define DEBUG
#define FINGER_PRINT


#define FILE_STORE
/*
#define PERIOD		120
#define CHANGEDET
*/

union DEF {
	int	defenum;
	int  	defint;
};

struct RGENUM {
	char 	array[MAXNUM][MAXTYPE]; 
	int 	array_guard;	
};

struct RGINT {
	int 	array[RGNUM];
};

union RG {
	struct RGINT  rgint;
	struct RGENUM rgenum;
};

struct DEP {
	int id;
	int methodid;
};
	
struct DIM {
	char 	id[MAXLEN];
	char 	subsys[MAXTYPE];
	char 	prop[MAXTYPE];
	char 	loc[MAXLEN];
	char 	type[MAXTYPE];
	union 	DEF  def;
	union 	RG   range;
	struct 	DEP dep;
};

struct VECTOR {
	struct DIM dim[MAXDIM];
	int dim_guard;
};
 
#ifdef SUBSYS_DISK
struct SNAPSHOT	{
	unsigned long reads;
	unsigned long writes;
	unsigned long read_sectors;
	unsigned long write_sectors;
	unsigned long time_in_queue;	
};

#ifdef FINGER_PRINT
struct fingerprint {
	int concurrency;
	int rw_ratio;
	unsigned long avg_size;	
	unsigned long avg_tt;
	unsigned long avg_dist;
};
#endif
#elif defined (SUBSYS_CPU)
struct SNAPSHOT {
	unsigned long instructions;	
};

#ifdef FINGER_PRINT
struct fingerprint {
	unsigned long instructions;	
};	
#endif	
#elif defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)

#ifdef FINGER_PRINT
struct fingerprint {
	unsigned long avg_lock_time;		
};
#endif
struct SNAPSHOT {
	unsigned long lock_time;
	unsigned long lock_cnt;
};		
#endif

#define MAX_PROCBUF_SIZE	256
static char proc_buffer[MAX_PROCBUF_SIZE];
static struct task_struct *task;	
struct VECTOR vector;
struct RECORDER record;

#ifdef FILE_STORE
struct RECORDER backup_rcd;
#endif

struct SNAPSHOT snapshot_before, snapshot_end;

struct module_controller {
	int module_enable;
	int module_disable;
} mc;

#define MAXDEP		2
struct dep {
	char buf[MAXDEP][MAXTYPE];
	int id;		
};	
struct dep dep_buf[MAXLEN];	

#ifdef PERIOD
#define PERIOD_STOP 	-1
int period_cnt = PERIOD_STOP;
#endif

#if (defined(SUBSYS_DISK) || defined(SUBSYS_CPU) || defined(SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)) && defined(FINGER_PRINT)
struct fp_snapshot fp_snapshot_before, fp_snapshot_end;

#define CACHE_SIZE	100
struct cache_element {
	struct fingerprint fp;
	struct RECORDER rcd;
};

struct PERFCACHE {
	struct cache_element perfc[CACHE_SIZE];
	int guard;
} perfcache;
#endif

extern void emergency_sync(void);

#ifdef SUBSYS_DISK
extern int IOCNT_THD;

#elif defined (SUBSYS_CPU) 
extern struct cpu_info cpu_sensor;	
extern volatile int sent_once;

#elif defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)

#ifdef SUBSYS_MIXEDLOCK
extern volatile int method_tuner;
#endif
extern volatile int val_tuner;
extern unsigned long total_lock_cnt;
extern unsigned long total_lock_time;
#endif

void *alloc_buffer(int len)
{
        return (void *)__get_free_pages(GFP_KERNEL, get_order(len));
}

void free_buffer(void *buffer, int len)
{
        free_pages((unsigned long)buffer, get_order(len));
}

static struct file *open_file (char const *file_name, int flags, int mode)
{
	struct file *file = NULL;
#if BITS_PER_LONG != 32
	flags |= O_LARGEFILE;
#endif
	file = filp_open(file_name, flags, mode);

        return file;
}

static void close_file(struct file *file)
{
        if (file->f_op && file->f_op->flush) {
                file->f_op->flush(file, current->files);
        }
        fput(file);
}

static int kernel_write(struct file *file, unsigned long offset, const char *addr, unsigned long count)
{
        mm_segment_t old_fs;
        loff_t pos = offset;
        int result = -ENOSYS;

        if (!file->f_op->write)
                goto fail;
        old_fs = get_fs();
        set_fs(get_ds());
        result = file->f_op->write(file, addr, count, &pos);
        set_fs(old_fs);
fail:
        return result;
}

static char *read_into_buffer(char *file_name, int *size)
{
	int ret, file_size, data_read = 0;
	struct file *file;
	void *buffer;
	
	file = open_file(file_name, O_RDONLY, 00777);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		printk("Cannot open %s for reading, error = %d\n", file_name, ret);
		return NULL;
	}
	
	file_size = file->f_mapping->host->i_size;
	if (file_size <=0) {
		printk("Empty file\n");
		return NULL;
	}	
	
	buffer = alloc_buffer(file_size + 1);
	memset(buffer, '\0', file_size + 1);
	
	if (NULL == buffer) {
		printk("Cannot allocate memory\n");
		close_file(file);
		return NULL;
	}	
	
	while (data_read < file_size) {
		
		ret = kernel_read(file, data_read, buffer + data_read, file_size - data_read);
			
		if (ret < 0) {
			printk("Error in reading file, error = %d\n", ret);
			goto out_close_free;
		} else if (0 == ret) {
			printk("File is too small\n");
			break;
		}
		data_read += ret;
	}
	close_file(file);
	*size = file_size + 1;

	return buffer;

out_close_free:	
	close_file(file);
	free_buffer(buffer, file_size + 1);

	return NULL;	
}

static void global_init(void)
{
	int i;

	vector.dim_guard = -1;
        
	for (i = 0; i<MAXDIM; i++) {
                vector.dim[i].dep.id = -1;
		vector.dim[i].dep.methodid = -1;	
	}	
        
	mc.module_enable = mc.module_disable = OFF;
	memset(proc_buffer, '\0', MAX_PROCBUF_SIZE);
	
#if (defined(SUBSYS_CPU) || defined(SUBSYS_DISK) || defined(SUBSYS_SPINLOCK) || defined(SUBSYS_MIXEDLOCK)) && defined (FINGER_PRINT)
	perfcache.guard = -1;
#endif
}

#ifdef FILE_STORE
static unsigned long get_checksum(char *buf);

static void cache_init(void)
{
	int size, specsize, offset, i, j;
	char *buf, *specbuf, *p;
	unsigned long stored_cs, calc_cs;

	buf = read_into_buffer("/etc/SPEC/cache", &size);
	if (NULL == buf) {
		printk("buf from cache file is null\n");
		return;
	}
	p = buf;
	sscanf(p, "%lu%n", &stored_cs, &offset);
	p += offset;

	specbuf = read_into_buffer("/etc/SPEC/spec", &specsize);
	if (NULL == specbuf) {
		printk("buf from spec file is null\n");
		return;	
	}
	calc_cs = get_checksum(specbuf);

	if (stored_cs != calc_cs) {
		return;
	}
#ifdef DEBUG
	else printk("found previous storage\n");
#endif

	for (i = 0; i< vector.dim_guard; i++) {
		if (1 != sscanf(p, "%d%n", &record.conf[i], &offset)) {
			printk("not enough info\n");
			return;
		}
#ifdef DEBUG
		printk("%d\t", record.conf[i]);
#endif
		p += offset;
	}
#ifdef DEBUG
	printk("\n");
#endif
	
	for (i = 0; i< vector.dim_guard; i++) {
		if (1 != sscanf(p, "%d%n", &record.bestconf[i], &offset)) {
			printk("not enough info\n");
			return;
		} 
#ifdef DEBUG
		printk("%d\t", record.bestconf[i]);
#endif
		p += offset;
	}
#ifdef DEBUG
	printk("\n");
#endif
	
	if (5 != sscanf(p, "%lu%d%d%d%d%n", &record.best, &record.completed, &record.dim_loc, &record.loc.enumloc, &record.to_value, &offset)) {
	
		printk("no record info from best\n");
		return;	
	} 
#ifdef DEBUG
	printk("%lu\t%d\t%d\n", record.best, record.completed, record.dim_loc);
	printk("%d\n", record.loc.enumloc);
	printk("%d\n", record.to_value);
#endif
	p += offset;

	if (1 != sscanf(p, "%d%n", &perfcache.guard, &offset)) {
		printk("no perfcache number\n");
		return;
	}
	p += offset;

	printk("guard = %d\n", perfcache.guard);

	for (i = 0; i <= perfcache.guard; i++) {
		if (5 != sscanf(p, "%d%d%lu%lu%lu%n", &perfcache.perfc[i].fp.concurrency, &perfcache.perfc[i].fp.rw_ratio, &perfcache.perfc[i].fp.avg_size, &perfcache.perfc[i].fp.avg_tt, &perfcache.perfc[i].fp.avg_dist, &offset)) {
			printk("not enough finger print info\n");
			return;
		}	
		p += offset;
#ifdef DEBUG
		printk("%d\t%lu\n", perfcache.perfc[i].fp.rw_ratio, perfcache.perfc[i].fp.avg_size);
#endif		
		for (j = 0; j < vector.dim_guard; j++) {
			if (1 != sscanf(p, "%d%n", &perfcache.perfc[i].rcd.conf[j], &offset)) {
				printk("not enough conf info\n");
				return;
			}
			p += offset;
#ifdef DEBUG
			printk("%d\t", perfcache.perfc[i].rcd.conf[j]);
#endif
		}
#ifdef DEBUG
		printk("\n");
#endif

		for (j = 0; j < vector.dim_guard; j++) {
			if (1 != sscanf(p, "%d%n", &perfcache.perfc[i].rcd.bestconf[j], &offset)) {
				printk("not enough bestconf info\n");
				return;
			}
			p += offset;
#ifdef DEBUG
			printk("%d\t", perfcache.perfc[i].rcd.bestconf[j]);
#endif
		}
#ifdef DEBUG
		printk("\n");
#endif

		if (5 != sscanf(p, "%lu%d%d%d%d%n", &perfcache.perfc[i].rcd.best, &perfcache.perfc[i].rcd.completed, &perfcache.perfc[i].rcd.dim_loc, &perfcache.perfc[i].rcd.loc.enumloc, &perfcache.perfc[i].rcd.to_value, &offset)) {
			printk("not enough info\n");
			return;
		}
		p += offset;
#ifdef DEBUG
		printk("%lu\t%d\t%d\n", perfcache.perfc[i].rcd.best, perfcache.perfc[i].rcd.completed, perfcache.perfc[i].rcd.dim_loc);
		printk("%d\n", perfcache.perfc[i].rcd.loc.enumloc);
		printk("%d\n", perfcache.perfc[i].rcd.to_value);
#endif
	}

	free_buffer(specbuf, specsize);
	free_buffer(buf, size);	
}
#endif /*#ifdef FILE_STORE*/ 

static int convert(char *p)
{
	char *t = p;	
	int sum = 0;

	if (NULL == t)
 		return -1;

	while ('\0' != *t) {
		if (*t >= '0' && *t <= '9') {	
			sum = 10*sum + ((*t) -'0');
			t ++;
		}
		else return -1;
	}	

	return sum;
}

static int enhanced_spec_parser(void)
{
	int i, j, size, loc, dim, cnt, iter, last_iter, pairs, correct_format, left_id, right_id;
	char *p, *q;
	char *buf = read_into_buffer("/etc/SPEC/spec", &size);
	char defenum[MAXTYPE];
	
	if (NULL == buf) {
		printk("Error in parsing /etc/SPEC/spec\n");
		return -EPERM;
	}

#ifdef DEBUG
	printk("buffer size = %d\n", size);
#endif
	loc = dim = pairs = 0; 

start_parse:
	if ('#' == buf[loc]) {
		loc ++;	
		while ('\n' != buf[loc] && '\0' != buf[loc]) 
			loc ++;
		if ('\0' == buf[loc]) 
			return 0;
		else {
			loc ++;
#ifdef DEBUG
			printk("AFTER REMOVING #\n%s", &buf[loc]);
#endif
			goto start_parse;
		}
	} else if ('\n' == buf[loc] || ' ' == buf[loc]) {
		loc ++;
		while ('\n' == buf[loc] || ' ' == buf[loc]) 
			loc ++;
#ifdef DEBUG
		printk("AFTER REMOVING space and empty lines\n%s", &buf[loc]);
#endif
		goto start_parse;

	} else if ('<' == buf[loc]) {
		loc ++;
		if ('D' == buf[loc]) {
			cnt = 0;
			iter = loc;
			last_iter = -1;
			while ('>' != buf[iter]) {
				iter ++;
				if ('\n' == buf[iter] || ' ' == buf[iter] || '\0' == buf[iter]) {
					printk("Invalid format\tonly <\n");
					goto spec_error;
				}
				if (':' == buf[iter]) {
					cnt++;
					if (-1 == last_iter) 
						last_iter = iter;
					else if (1 == iter - last_iter) {
						printk("Invalid format\tEmpty Domain\n");
						goto spec_error;
					}
				}	
			} 
			if (TOTAL_DOM != cnt) {
				printk("Invalid format\t#domains %d are not TOTAL_DOM\n%s", cnt, &buf[loc]);
				goto spec_error;
			} 
			p = q = &buf[loc + 2];
			cnt = 0;
			while ('>' != *q) {
				if (':' == *q) {
					cnt ++;	
					*q = '\0';
					switch (cnt) {
						case DOM_ID: 
							memset(vector.dim[dim].id, '\0', sizeof(vector.dim[dim].id));
			                                strcpy(vector.dim[dim].id, p);
							break;
						case DOM_SUBSYS:
							memset(vector.dim[dim].subsys, '\0', sizeof(vector.dim[dim].subsys));
			                                strcpy(vector.dim[dim].subsys, p);
							break;
						case DOM_PROP:
							memset(vector.dim[dim].prop, '\0', sizeof(vector.dim[dim].prop));
			                                strcpy(vector.dim[dim].prop, p);
							break;
						case DOM_LOC:
							memset(vector.dim[dim].loc, '\0', sizeof(vector.dim[dim].loc));
			                                strcpy(vector.dim[dim].loc, p);
							break;
						case DOM_TYPE:	
							memset(vector.dim[dim].type, '\0', sizeof(vector.dim[dim].type));
			    	                        strcpy(vector.dim[dim].type, p);
							break;
		                                case DOM_DEF:
  					                if (0 == strcmp("enum", vector.dim[dim].type)) {
                                        			memset(defenum, '\0', sizeof(defenum));
                                        			strcpy(defenum, p);
                                			} else 	vector.dim[dim].def.defint = convert(p);
                                			break;
					  	default: break; 
					}
					q++; p = q;	
				} else {
					if (DOM_DEF == cnt)
						break;
					q++;
				}
			}
			
   			cnt = 0;
			while ('>' != *q) {
				if (',' == *q) {
					*q = '\0';
					if (0 == strlen(p)) {
						printk("Invalid format\tEmpty domain\n");
						goto spec_error;
					}
					if (0 == strcmp("enum", vector.dim[dim].type)) {
					        memset(vector.dim[dim].range.rgenum.array[cnt], '\0', sizeof(vector.dim[dim].range.rgenum.array[cnt]));
						strcpy(vector.dim[dim].range.rgenum.array[cnt], p);
					} else vector.dim[dim].range.rgint.array[cnt] = convert(p);
					cnt ++; q++; p = q;
				} else q++;
			}
			
			*q = '\0';
			if (0 == strlen(p)) {
				printk("Invalid format\tEmpty domain\n");
				goto spec_error;
			}
	
			if (0 == strcmp("enum", vector.dim[dim].type)) {
 	   			memset(vector.dim[dim].range.rgenum.array[cnt], '\0', sizeof(vector.dim[dim].range.rgenum.array[cnt]));
				strcpy(vector.dim[dim].range.rgenum.array[cnt], p);
				vector.dim[dim].range.rgenum.array_guard = cnt + 1;
                        } else  vector.dim[dim].range.rgint.array[cnt] = convert(p);
			
                        if (0 == strcmp("enum", vector.dim[dim].type)) {
                               for (i =0; i <= vector.dim[dim].range.rgenum.array_guard; i++) {
					if (0 == strcmp(defenum, vector.dim[dim].range.rgenum.array[i])) {
                                        	vector.dim[dim].def.defenum = i; break;
                                        }
                               }
                        }
			dim ++;
			loc = iter + 1;
#ifdef DEBUG
			printk("D section\n%s", &buf[loc]);
#endif			
		  	goto start_parse;
		        	
		} else if ('C' == buf[loc]) {
			if (':' != buf[loc + 1]) {
				printk("Invalid format\tno :\n");
				goto spec_error;
			}
			p = &buf[loc + 2]; q = p; correct_format = 0;
			while ('\n' != *q && '\0' != *q) {
				if (':' == *q) {
					*q = '\0';
					if (0 == strlen(p)) {
						printk("Invalid format\tempty domain\n");
						goto spec_error;
					}
					memset(dep_buf[pairs].buf[0], '\0', MAXTYPE);
					strcpy(dep_buf[pairs].buf[0], p);
					q ++; p = q;
				} else if (',' == *q) {
					*q = '\0';
					if (0 == strlen(p)) {
						printk("Invalid format\tempty domain\n");
						goto spec_error;
					}
					memset(dep_buf[pairs].buf[1], '\0', MAXTYPE);
					strcpy(dep_buf[pairs].buf[1], p);
					q++; p = q;
				} else if ('>' == *q) {
					*q = '\0';
					if (0 == strlen(p)) {
						printk("Invalid format\tempty domain\n");
						goto spec_error;
					}
					dep_buf[pairs].id = convert(p);
					pairs++;	
					correct_format = 1;				
					break;	
				} else q++;
			}	
			if (0 == correct_format) {
				printk("Invalid format\tno >\n");
				goto spec_error;	
			} 
			loc += q - &buf[loc] + 1;
#ifdef DEBUG
			printk("C section\n%s", &buf[loc]);
#endif
			goto start_parse;						
		} else {
			printk("Invalid domain\tnot C or D at first domain\nit is <%s>\n", &buf[loc]);
			goto spec_error;
		} 		
	} else {
		if ('\0' != buf[loc]) {
			printk("Invalid format\tinvalid first char\nit is <%s>", &buf[loc]);
			goto spec_error;
		} 
	}	
	for (i = 0; i < pairs; i++) {
		left_id = right_id = -1;
		for (j = 0; j < dim; j++) {
			if (0 == strcmp(dep_buf[i].buf[0], vector.dim[j].id))
                                left_id = j;
			if (0 == strcmp(dep_buf[i].buf[1], vector.dim[j].id))
				right_id = j;
		}
		if (-1 == left_id || -1 == right_id) {
			printk("Invalid format\tid in dependance cannot be found in description\n");
			goto spec_error;	
		}
		vector.dim[left_id].dep.id = right_id;
		vector.dim[left_id].dep.methodid = dep_buf[i].id;	
	}

        vector.dim_guard = dim;
	free_buffer(buf, size);
#ifdef DEBUG                    
        for (i=0; i < vector.dim_guard; i++) {
                if (0 == strcmp("enum", vector.dim[i].type)) {
                        printk("DIM=%d\tID=%s\tSUBSYS=%s\tPROP=%s\tLOC=%s\tTYPE=%s\tDEPID=%d\tDEPMID=%d\tDEF=%d\n", i, vector.dim[i].id, vector.dim[i].subsys, vector.dim[i].prop,vector.dim[i].loc, vector.dim[i].type, vector.dim[i].dep.id, vector.dim[i].dep.methodid, vector.dim[i].def.defenum);
                        for (j=0; j< vector.dim[i].range.rgenum.array_guard; j++) {
                                printk("%d\t%s\n", j, vector.dim[i].range.rgenum.array[j]);
                        }
                } else {
                        printk("DIM=%d\tID=%s\tSUBSYS=%s\tPROP=%s\tLOC=%s\tTYPE=%s\tDEPID=%d\tDEPMID=%d\tDEF=%d\n", i, vector.dim[i].id, vector.dim[i].subsys, vector.dim[i].prop,vector.dim[i].loc, vector.dim[i].type, vector.dim[i].dep.id, vector.dim[i].dep.methodid, vector.dim[i].def.defint);
                        for (j=0; j<3; j++) {
                                if (0 == j)
                                        printk("MIN\t%d\n", vector.dim[i].range.rgint.array[j]);
                                else if (1 == j)
                                        printk("MAX\t%d\n", vector.dim[i].range.rgint.array[j]);
                                else
                                        printk("STEP\t%d\n", vector.dim[i].range.rgint.array[j]);
                        }
                }
        }
#endif
	return 0;

spec_error:
	free_buffer(buf, size);
	return -1;
}

static int echo_to_procfs(char degree)
{
	struct file *file;
	int ret;

	file = open_file("/proc/sys/vm/drop_caches", O_RDWR, 0777);
	
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		printk("cannot open /proc/sys/vm/drop_caches to write, error=%d\n", ret);
		return -1;
	}	

	ret = kernel_write(file, 0, &degree, 1);
	if (ret < 0) {
		printk("cannot writing to /proc/sys/vm/drop_caches\n");
		return -1;
	}

	close_file(file);

	return 0;
}

static int echo_to_sysfs(void)
{
	int i, ret, cnt;
	char buffer[50];

#ifdef DEBUG
	printk("ECHO TO SYSFS\n");
	for (i =0; i < vector.dim_guard; i++) {
		printk("%d\t%d\n", i, record.conf[i]);
	}			
#endif

	for (i = 0; i < vector.dim_guard; i++) {

		if (record.conf[i] >= 0) {
			struct file *file;
			file = open_file(vector.dim[i].loc, O_RDWR, 0600);

			if (IS_ERR(file)) {
				ret = PTR_ERR(file);
				printk("cannot open %s, error=%d\n", vector.dim[i].loc, ret);
				return -1;
			}
			if (0 == strcmp("enum", vector.dim[i].type)) {
				ret = kernel_write(file, 0, vector.dim[i].range.rgenum.array[record.conf[i]], MAXTYPE);
				if (ret < 0) 
					printk("error when writing %s to %s, error = %d\n", vector.dim[i].range.rgenum.array[record.conf[i]], vector.dim[i].loc, ret);
			}
			else {
				memset(buffer, '\0', sizeof(buffer));	
				sprintf(buffer, "%d", record.conf[i]);
				
				cnt = 0;
				while (buffer[cnt] != '\0') cnt++;
	
				ret = kernel_write(file, 0, buffer, cnt);

				if (ret < 0) 
					printk("error when writing %s to %s, error = %d\n", buffer, vector.dim[i].loc, ret);
			}
		
			close_file(file);
		}	
	}

	return 0;	
}

static void perf_recorder_init(void)
{
	int i;

	record.best = 0UL;
	record.completed = 0;
	record.dim_loc = vector.dim_guard - 1;
	record.loc.enumloc = -1;	
	record.to_value = 0;
	
	for (i = 0; i < vector.dim_guard; i++) {
		if (0 == strcmp("method", vector.dim[i].prop)) 
			record.conf[i] = 0;
		else if (0 == strcmp("int", vector.dim[i].type))
			record.conf[i] = vector.dim[i].def.defint;
		else 
			record.conf[i] = vector.dim[i].def.defenum;	
	
		if (-1 != vector.dim[i].dep.id && 0 != vector.dim[i].dep.methodid)
			record.conf[i] = -1;
	}
#if defined (SUBSYS_CPU) 
	if (0 == sent_once) { 
		sent_once = 1;
		for (i = 0; i < MAXCORES; i++) 
			cpu_sensor.sensor[i].start_to_monitor = START_SIGNALED;			
	}
#elif defined(SUBSYS_SPINLOCK) || defined(SUBSYS_MIXEDLOCK)
	val_tuner = 0;
#ifdef SUBSYS_MIXEDLOCK
	method_tuner = 0;
#endif
#endif	
}

static void update_record(void)
{
	int i;
	
	for (i = 0; i < vector.dim_guard; i++) {
		if (0 == strcmp("value", vector.dim[i].prop)) {
			if (-1 != vector.dim[i].dep.id && vector.dim[i].dep.methodid == record.conf[vector.dim[i].dep.id]) {
        	        	if (0 == strcmp("enum", vector.dim[i].type)) 
                	        	record.conf[i] = vector.dim[i].def.defenum;
                       		else 
                                	record.conf[i] = vector.dim[i].def.defint;                      
                	} else if (-1 != vector.dim[i].dep.id)
				record.conf[i] = -1;
		}
        }
}

static void orthg_search(unsigned long metric, int ctl) 
{
	int value_found, method_found, i;

	if (AUTO_COMPLETED == record.completed) {
#ifdef CHANGEDET
		if (metric < record.best/2 || metric > 2*record.best) 
			perf_recorder_init();					
#endif
		return;
	}

	if (0 == record.best) {
#ifdef DEBUG
	printk("update directly, best = %lu\n", metric);
#endif
		record.best = metric;
		for (i = 0; i < vector.dim_guard; i++) 
			record.bestconf[i] = record.conf[i];
	}
	else if ((1 == ctl && metric > record.best) || (0 == ctl && metric < record.best)) {
#ifdef DEBUG
	printk("better, update best, best = %lu\n", metric);
#endif
		record.best = metric;
		for (i = 0; i < vector.dim_guard; i++) 
			record.bestconf[i] = record.conf[i];	
	}

	value_found = method_found = 0;	
	for (;;) {

		if (0 == record.to_value) {
			if (0 == strcmp("method", vector.dim[record.dim_loc].prop)) {
				if (record.conf[record.dim_loc] == vector.dim[record.dim_loc].range.rgenum.array_guard - 1) {
					record.conf[record.dim_loc] = record.bestconf[record.dim_loc];							
					update_record();
				}
				else {
					record.conf[record.dim_loc]++;
					update_record();
					method_found = 1;					
					break;
				}				
			}
			if (0 == record.dim_loc) {
				record.dim_loc = vector.dim_guard - 1;
				record.to_value = 1;
			} else record.dim_loc--;

		} else {
			if (-1 != record.conf[record.dim_loc] && 0 == strcmp("value", vector.dim[record.dim_loc].prop)) {
				if (0 == strcmp("int", vector.dim[record.dim_loc].type)) {

					if (vector.dim[record.dim_loc].range.rgint.array[0] == vector.dim[record.dim_loc].range.rgint.array[1])
						record.conf[record.dim_loc] = record.bestconf[record.dim_loc];
					
					else if (vector.dim[record.dim_loc].def.defint == vector.dim[record.dim_loc].range.rgint.array[1]) {
						if (record.conf[record.dim_loc] == vector.dim[record.dim_loc].range.rgint.array[0])
							record.conf[record.dim_loc] = record.bestconf[record.dim_loc];
						else {
#ifndef CONFIG_STEP
							record.conf[record.dim_loc] /= 2;
#else
							record.conf[record.dim_loc] -= vector.dim[record.dim_loc].range.rgint.array[2];
#endif
							if (record.conf[record.dim_loc] <= vector.dim[record.dim_loc].range.rgint.array[0])
								record.conf[record.dim_loc] = vector.dim[record.dim_loc].range.rgint.array[0];
							value_found = 1;
						
							break;
						}
					} else {
						if (record.conf[record.dim_loc] == vector.dim[record.dim_loc].range.rgint.array[1]) 
							record.conf[record.dim_loc] = record.bestconf[record.dim_loc]; 
						else {
							if (record.conf[record.dim_loc] == vector.dim[record.dim_loc].range.rgint.array[0]) {
#ifndef CONFIG_STEP
								if (0 == vector.dim[record.dim_loc].def.defint)
									record.conf[record.dim_loc] = 1;
								else
									record.conf[record.dim_loc] = 2 * vector.dim[record.dim_loc].def.defint;
#else
								record.conf[record.dim_loc] = vector.dim[record.dim_loc].def.defint + vector.dim[record.dim_loc].range.rgint.array[2];
#endif
							}
							else {	

								if (record.conf[record.dim_loc] <= vector.dim[record.dim_loc].def.defint) {
#ifndef CONFIG_STEP
									record.conf[record.dim_loc] /= 2; 
#else
									record.conf[record.dim_loc] -= vector.dim[record.dim_loc].range.rgint.array[2];
#endif
								}
								else {
#ifndef CONFIG_STEP
									record.conf[record.dim_loc] *= 2;
#else
									record.conf[record.dim_loc] += vector.dim[record.dim_loc].range.rgint.array[2];
#endif
								}
							}	
	
							if (record.conf[record.dim_loc] <= vector.dim[record.dim_loc].range.rgint.array[0]) 
								record.conf[record.dim_loc] = vector.dim[record.dim_loc].range.rgint.array[0];
						
							else if (record.conf[record.dim_loc] > vector.dim[record.dim_loc].range.rgint.array[1])
								record.conf[record.dim_loc] = vector.dim[record.dim_loc].range.rgint.array[1];

							value_found = 1;
							break;
						}
					}
				} else {
					if (1 == vector.dim[record.dim_loc].range.rgenum.array_guard || record.loc.enumloc == vector.dim[record.dim_loc].range.rgenum.array_guard - 1) {
						record.conf[record.dim_loc] = record.bestconf[record.dim_loc];
						record.loc.enumloc = -1;
					} else {
						record.loc.enumloc ++;
						if (vector.dim[record.dim_loc].def.defenum != record.loc.enumloc) {
							record.conf[record.dim_loc] = record.loc.enumloc;
							value_found = 1;
							break;
						} else {
							if (record.loc.enumloc == vector.dim[record.dim_loc].range.rgenum.array_guard - 1) {
								record.conf[record.dim_loc] = record.bestconf[record.dim_loc];
								record.loc.enumloc = -1;
							} else {
								record.loc.enumloc++;
								record.conf[record.dim_loc] = record.loc.enumloc;
								value_found = 1;
								break;
							}
						}
					}
				}
			}
			if (0 == record.dim_loc) 
				break;					
			else record.dim_loc --;	
		}	 
	}
	
	if (0 == value_found && 0 == method_found) {
		record.completed = AUTO_COMPLETED;
#ifdef PERIOD		
		if (-1 == period_cnt) 
			period_cnt = 0;
#endif

		for (i = 0; i < vector.dim_guard; i++) 
			record.conf[i] = record.bestconf[i];				
#ifdef DEBUG
		printk("COMPLETED\n");
		printk("training finished, selected = %d\n", record.conf[0]);
#endif
	}
		
#ifdef FILE_STORE
        backup_rcd = record;
#endif

	return;	
}


static void search_engine(unsigned long metric, int ctl)
{
	orthg_search(metric, ctl);
}

#if defined(SUBSYS_DISK) && defined(FINGER_PRINT)
static void consolidate_fp_snapshot(struct fp_snapshot *master, struct fp_snapshot *instance);
#endif

#ifdef SUBSYS_DISK
extern struct list_head gendisks;

void clear_ids(void) 
{
	struct list_head *d;
	struct gendisk *disk;
	int i;
	
	list_for_each(d, &gendisks) {

		disk = list_entry(d, struct gendisk, gendisks);
		write_seqlock(&disk->seqlock);
		disk->fp_ss.recorder.guard = -1;
		for (i = 0; i < MAXIDS; i++) 
			disk->fp_ss.recorder.id_array[i] = -1;	
		write_sequnlock(&disk->seqlock);
	}
}
#endif

#ifdef SUBSYS_DISK
#ifdef FINGER_PRINT
unsigned long get_disk_stats(struct SNAPSHOT *p, struct fp_snapshot *fps)
#else
unsigned long get_disk_stats(struct SNAPSHOT *p)
#endif
{
	struct list_head *d;
	struct gendisk *disk;
	struct disk_part_iter piter;
	struct hd_struct *hd;
	struct fp_snapshot fps_copy;
	int cpu, seq;

#ifdef FINGER_PRINT
	memset(fps, 0, sizeof(struct fp_snapshot));
#endif
	list_for_each(d, &gendisks) {

		disk = list_entry(d, struct gendisk, gendisks);
#ifdef FINGER_PRINT
		do {
			seq = read_seqbegin(&disk->seqlock);
			fps_copy = disk->fp_ss; 	
		} while (read_seqretry(&disk->seqlock, seq));
		
		consolidate_fp_snapshot(fps, &fps_copy);			
#endif		
		disk_part_iter_init(&piter, disk, DISK_PITER_INCL_EMPTY_PART0);
		while ((hd = disk_part_iter_next(&piter))) {
			cpu = part_stat_lock();
			part_round_stats(cpu, hd);
			part_stat_unlock();	
			p->reads += part_stat_read(hd, ios[READ]);
			p->writes += part_stat_read(hd, ios[WRITE]);				
			p->read_sectors += part_stat_read(hd, sectors[READ]);
			p->write_sectors += part_stat_read(hd, sectors[WRITE]);
			p->time_in_queue += part_stat_read(hd, time_in_queue);
		}			 				
	}

        return 0;
}
#elif defined (SUBSYS_CPU)
#ifdef FINGER_PRINT
unsigned long get_cpu_stats(struct SNAPSHOT *p, struct fp_snapshot *fps)
#else
unsigned long get_cpu_stats(struct SNAPSHOT *p)
#endif
{
	int i, seq;
	unsigned long cnt_copy;
	unsigned long sum = 0UL;
	
	
	for (i = 0; i < MAXCORES; i++) {
		do {
			seq = read_seqbegin(&(cpu_sensor.sensor[i].lock));
			cnt_copy = cpu_sensor.sensor[i].cnt;
		} while	(read_seqretry(&(cpu_sensor.sensor[i].lock), seq));

		sum += cnt_copy;
	}

	p->instructions = sum;
#ifdef FINGER_PRINT
	fps->instructions = sum;
#endif
	
	return 0;
}
#elif defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)
#ifdef FINGER_PRINT
unsigned long get_spinlock_stats(struct SNAPSHOT *p, struct fp_snapshot *fps)
#else
unsigned long get_spinlock_stats(struct SNAPSHOT *p)
#endif
{
	p->lock_cnt = total_lock_cnt;
	p->lock_time = total_lock_time;

#ifdef FINGER_PRINT
	memset(fps, 0, sizeof(struct fp_snapshot));
	fps->lock_cnt = total_lock_cnt;
	fps->lock_time = total_lock_time;
#endif

	return 0;
}
#endif

#ifdef SUBSYS_DISK
static void snapshot_init(struct SNAPSHOT *p)
{
	p->reads = p->writes = 0UL;
	p->read_sectors = p->write_sectors = 0UL;
	p->time_in_queue = 0UL;
}
#elif defined (SUBSYS_CPU)
static void snapshot_init(struct SNAPSHOT *p)
{
	p->instructions = 0UL;
}
#elif defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)
static void snapshot_init(struct SNAPSHOT *p)
{
	p->lock_time = 0UL;
	p->lock_cnt = 0UL;
}
#endif

#ifdef SUBSYS_DISK
static unsigned long disk_ops_calc(struct SNAPSHOT *before, struct SNAPSHOT *end)
{
	if (end->writes >= before->writes && end->reads >= before->reads) {
		return end->writes - before->writes + end->reads - before->reads;
	} else {
		printk("BUG\n");
		return 0;
	}
}

static unsigned long disk_thoughput_calc(struct SNAPSHOT *before, struct SNAPSHOT *end)
{
	if (end->write_sectors >= before->write_sectors && end->read_sectors >= before->read_sectors) {
		return end->write_sectors - before->write_sectors + end->read_sectors - before->read_sectors;
	} else {
		printk("BUG\n");
		return 0;
	}
}

static unsigned long disk_latency_calc(struct SNAPSHOT *before, struct SNAPSHOT *end)
{
	if (end->time_in_queue >= before->time_in_queue) {
		return end->time_in_queue - before->time_in_queue;
	} else {
		printk("BUG\n");
		return 0;
	}
}
#elif defined(SUBSYS_CPU) 
static unsigned long cpu_instructions_calc(struct SNAPSHOT *before, struct SNAPSHOT *end)
{
	if (end->instructions >= before->instructions) 
		
		return end->instructions - before->instructions;
	else {
		printk("BUG\n");
		
		return 0;
	}
}
#elif defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)
static unsigned long spinlock_time_calc(struct SNAPSHOT *before, struct SNAPSHOT *end)
{
        if (end->lock_cnt > before->lock_cnt && end->lock_time >= before->lock_time)

                return (end->lock_time - before->lock_time)/(end->lock_cnt - before->lock_cnt);
        else {
                printk("BUG\n");

                return 0;
        }
}
#endif

#if defined(SUBSYS_DISK) && defined(FINGER_PRINT)
static void consolidate_fp_snapshot(struct fp_snapshot *master, struct fp_snapshot *instance)
{
#ifdef DEBUG
	int i;
#endif
	master->reads += instance->reads;
	master->writes += instance->writes;
	master->total_size += instance->total_size;		
	master->total_tt += instance->total_tt;
	master->total_dist += instance->total_dist;
	master->recorder.guard += instance->recorder.guard + 1;
#ifdef DEBUG
	if (instance->recorder.guard >= 0) {
		printk("%d\n", instance->recorder.guard);
		for (i = 0; i <= instance->recorder.guard; i++) {
			printk("%d\t", instance->recorder.id_array[i]);
		}
		printk("\n");	
	}
#endif
}

static void cal_fp(struct fingerprint *fp, struct fp_snapshot *fp_ss)
{
	memset(fp, 0, sizeof(struct fingerprint));
	if (0 != fp_ss->reads + fp_ss->writes) {
		fp->rw_ratio = (100*fp_ss->reads)/(fp_ss->reads + fp_ss->writes);
		fp->avg_size = fp_ss->total_size/(fp_ss->reads + fp_ss->writes);	
 		fp->avg_tt = fp_ss->total_tt/(fp_ss->reads + fp_ss->writes);
		fp->avg_dist = fp_ss->total_dist/(fp_ss->reads + fp_ss->writes);
		fp->concurrency = fp_ss->recorder.guard;
#ifdef DEBUG 
		printk("rw_ratio=%d\tavg_size=%lu\tavg_tt=%lu\tavg_dist=%lu\ttotal_ids=%d\n", fp->rw_ratio, fp->avg_size, fp->avg_tt, fp->avg_dist, fp->concurrency);
#endif
	} else printk("reads and writes are both zero\n");
}

static void cal_fp_snapshot_diff(struct fp_snapshot *fp)
{
	memset(fp, 0, sizeof(struct fp_snapshot));

	if (fp_snapshot_end.reads >= fp_snapshot_before.reads)
		fp->reads = fp_snapshot_end.reads - fp_snapshot_before.reads;
	else printk("BUG when calc reads\n");

	if (fp_snapshot_end.writes >= fp_snapshot_before.writes)
		fp->writes = fp_snapshot_end.writes - fp_snapshot_before.writes;
	else printk("BUG when calc writes\n");

	if (fp_snapshot_end.total_size >= fp_snapshot_before.total_size)
		fp->total_size = fp_snapshot_end.total_size - fp_snapshot_before.total_size;
	else printk("BUG when calc total size");	

	if (fp_snapshot_end.total_tt >= fp_snapshot_before.total_tt) 
		fp->total_tt = fp_snapshot_end.total_tt - fp_snapshot_before.total_tt;
	else printk("BUG when calc total tt");

	if (fp_snapshot_end.total_dist >= fp_snapshot_before.total_dist)
		fp->total_dist = fp_snapshot_end.total_dist - fp_snapshot_before.total_dist;
	else printk("BUG when calc total dist");

	fp->recorder.guard = fp_snapshot_end.recorder.guard;
}

static int hit_cache(struct fingerprint fp, struct fingerprint cached_fp)
{
	if (fp.concurrency < 4*cached_fp.concurrency/5 || fp.concurrency > 6*cached_fp.concurrency/5) {
		printk("concurrency does not match\nfp.concurrency=%d\tcfp.concurrency=%d\n", fp.concurrency, cached_fp.concurrency);
		return 0;
	}

	if (fp.rw_ratio < 4*cached_fp.rw_ratio/5 || fp.rw_ratio > 6*cached_fp.rw_ratio/5) {
		printk("rw_ratio does not match\nfp.rw_ratio=%d\tcfp.rw_ratio=%d\n", fp.rw_ratio, cached_fp.rw_ratio);
		return 0;
	}

	if (fp.avg_size < 4*cached_fp.avg_size/5 || fp.avg_size > 6*cached_fp.avg_size/5) {
		printk("avg size does not match\nfp.avg_size=%lu\tcfp.avg_size=%lu\n", fp.avg_size, cached_fp.avg_size);	
		return 0;
	}

	if (fp.avg_tt < cached_fp.avg_tt/2 || fp.avg_tt > 2*cached_fp.avg_tt) {
		printk("avg tt does not match\nfp.avg_tt=%lu\tcfp.avg_tt=%lu\n", fp.avg_tt, cached_fp.avg_tt);
		return 0;
	}

	if (fp.avg_dist < cached_fp.avg_dist/2 || fp.avg_dist > 2*cached_fp.avg_dist) {
		printk("avg dist does not match\nfp.avg_dist=%lu\tcfp.avg_dist=%lu\n", fp.avg_dist, cached_fp.avg_dist);
		return 0;
	}
			
	return 1;
}
#elif (defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)) && defined (FINGER_PRINT)
static void cal_fp_snapshot_diff(struct fp_snapshot *fp)
{
        memset(fp, 0, sizeof(struct fp_snapshot));

        if (fp_snapshot_end.lock_cnt > fp_snapshot_before.lock_cnt)
                fp->lock_cnt = fp_snapshot_end.lock_cnt - fp_snapshot_before.lock_cnt;
        else printk("BUG when calc lock_cnt\n");

        if (fp_snapshot_end.lock_time >= fp_snapshot_before.lock_time)
                fp->lock_time = fp_snapshot_end.lock_time - fp_snapshot_before.lock_time;
        else printk("BUG when calc lock_time\n");
}

static void cal_fp(struct fingerprint *fp, struct fp_snapshot *fp_ss)
{
        memset(fp, 0, sizeof(struct fingerprint));
        if (0 != fp_ss->lock_cnt) {
                fp->avg_lock_time = fp_ss->lock_time/fp_ss->lock_cnt;
#ifdef DEBUG 
                printk("avg_lock_time = %lu\n", fp->avg_lock_time);
#endif
        } else printk("lock_cnt is zero\n");
}

static int hit_cache(struct fingerprint fp, struct fingerprint cached_fp)
{
        if (fp.avg_lock_time < 4*cached_fp.avg_lock_time/5 || fp.avg_lock_time > 6*cached_fp.avg_lock_time/5) {
                printk("avg_lock_time does not match\nfp.avg_lock_time = %lu\tcfp.avg_lock_time = %lu\n", fp.avg_lock_time, cached_fp.avg_lock_time);

                return 0;
        }

        return 1;
}
#elif defined(SUBSYS_CPU) && defined(FINGER_PRINT)
static void cal_fp_snapshot_diff(struct fp_snapshot *fp)
{
	memset(fp, 0, sizeof(struct fp_snapshot));
	
	if (fp_snapshot_end.instructions > fp_snapshot_before.instructions)
		fp->instructions = fp_snapshot_end.instructions - fp_snapshot_before.instructions;
	else printk("BUG when cal instructions\n");
}

static void cal_fp(struct fingerprint *fp, struct fp_snapshot *fp_ss)
{
	memset(fp, 0, sizeof(struct fingerprint));
	if (0 != fp_ss->instructions) {
		fp->instructions = fp_ss->instructions;
#ifdef DEBUG
		printk("instructions = %lu\n", fp->instructions);
#endif
	} else printk("instructions is zero\n");
}

static int hit_cache(struct fingerprint fp, struct fingerprint cached_fp) 
{
	if (fp.instructions < 4*cached_fp.instructions/5 || fp.instructions > 6*cached_fp.instructions/5) {
		printk("instructions does not match\nfp.instructions = %lu\tcfp.instructions = %lu\n", fp.instructions, cached_fp.instructions);
		return 0;
	}
	return 1;
}
#endif


#if  defined(SUBSYS_CPU) || defined(SUBSYS_SPINLOCK) || defined(SUBSYS_MIXEDLOCK)
static int kthread_work_cpu(void *data)
{
	unsigned long metric;
#ifdef FINGER_PRINT
	struct fp_snapshot fp_ss;
	struct fingerprint fp, stored_fp;
	int i, found;
#endif
	int not_cal = 1, first_cal = 1;
	snapshot_init(&snapshot_before);

#ifdef SUBSYS_CPU
#ifdef FINGER_PRINT
	get_cpu_stats(&snapshot_before, &fp_snapshot_before);
#else
	get_cpu_stats(&snapshot_before);	
#endif

#elif defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)
#ifdef FINGER_PRINT
	get_spinlock_stats(&snapshot_before, &fp_snapshot_before);
#else
	get_spinlock_stats(&snapshot_before);
#endif
#endif

	allow_signal(SIGKILL);
		
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(msecs_to_jiffies(CONFIG_LIFE_TIME));

		snapshot_init(&snapshot_end);

#ifdef SUBSYS_CPU
#ifdef FINGER_PRINT
		get_cpu_stats(&snapshot_end, &fp_snapshot_end);
#else
		get_cpu_stats(&snapshot_end);
#endif
#elif defined(SUBSYS_SPINLOCK) || defined(SUBSYS_MIXEDLOCK)
#ifdef FINGER_PRINT
		get_spinlock_stats(&snapshot_end, &fp_snapshot_end);
#else
		get_spinlock_stats(&snapshot_end);
#endif
#endif

#ifdef SUBSYS_CPU
		metric = cpu_instructions_calc(&snapshot_before, &snapshot_end);		
#elif defined(SUBSYS_SPINLOCK) || defined(SUBSYS_MIXEDLOCK)
		metric = spinlock_time_calc(&snapshot_before, &snapshot_end);
#endif

#ifdef DEBUG
		printk("metric = %lu\n", metric);
#endif

#ifdef SUBSYS_CPU
		search_engine(metric, 1);
#elif defined(SUBSYS_SPINLOCK) || defined(SUBSYS_MIXEDLOCK)
		search_engine(metric, 0);
#endif

#ifdef FINGER_PRINT
                cal_fp_snapshot_diff(&fp_ss);
                cal_fp(&fp, &fp_ss);
#endif
		
		if (1 == first_cal) {
#ifdef DEBUG
			printk("FIRST CALL\n");
#endif

#ifdef FINGER_PRINT
			found = 0;
			for (i = 0; i <= perfcache.guard; i++) {
				if (1 == hit_cache(fp, perfcache.perfc[i].fp)) {
#ifdef DEBUG
					printk("FOUND PREVIOUS RESULTS\n");
#if defined(SUBSYS_SPINLOCK) || defined(SUBSYS_MIXEDLOCK)
					printk("fp.average_lock_time = %lu\n", fp.avg_lock_time);
					printk("cfp.average_lock_time = %lu\n", perfcache.perfc[i].fp.avg_lock_time);
#elif defined (SUBSYS_CPU)
					printk("fp.instructions = %lu\n", fp.instructions);
					printk("cfp.instructions = %lu\n", perfcache.perfc[i].fp.instructions);
#endif
#endif
					record = perfcache.perfc[i].rcd;
					found = 1;
					if (AUTO_COMPLETED == record.completed) 
						not_cal = 2;
					else 
						not_cal = 1;
					break;
				}
			}
			stored_fp = fp;
			if (0 == found) 
				printk("NOT FOUND PREVIOUS RESULTS, CONTINUE TO SEARCH\n");	
#endif
			first_cal = 0;
			echo_to_sysfs();
			
		} else if (AUTO_COMPLETED == record.completed && 1 == not_cal) {
#ifdef FINGER_PRINT
			found = 0;
			for (i = 0; i <= perfcache.guard; i++) {
				if (1 == hit_cache(stored_fp, perfcache.perfc[i].fp) && AUTO_COMPLETED != perfcache.perfc[i].rcd.completed) {
					printk("FOUND PREVIOUS UNCOMPLETED RUN, REPLACE\n");
					perfcache.perfc[i].fp = stored_fp;	
					perfcache.perfc[i].rcd = record;
					found = 1;
					break;
				}	
			}

			if (0 == found && perfcache.guard < CACHE_SIZE - 1) {
				perfcache.guard ++;
				perfcache.perfc[perfcache.guard].fp = stored_fp;
				perfcache.perfc[perfcache.guard].rcd = record;
			}
			else printk("FOUND OR NO SLOT TO STORE\n");
#endif
			not_cal = 0;
			echo_to_sysfs();			

		} else if (AUTO_COMPLETED != record.completed) {
#ifdef FINGER_PRINT
			if (kthread_should_stop()) {
				found = 0;
				for (i = 0; i <= perfcache.guard; i++) {
					if (1 == hit_cache(stored_fp, perfcache.perfc[i].fp) && AUTO_COMPLETED != perfcache.perfc[i].rcd.completed) {
						printk("FOUND PREVIOUS UNCOMPLETED RUN, REPLACE\n");
						perfcache.perfc[i].fp = stored_fp;
						perfcache.perfc[i].rcd = record;
						found = 1;
						break;
					}
				}

				if (0 == found && perfcache.guard < CACHE_SIZE - 1) {
					printk("STORE UNCOMPLETED RUN\n");
					perfcache.guard ++;
					perfcache.perfc[perfcache.guard].fp = stored_fp;
					perfcache.perfc[perfcache.guard].rcd = record;
				}
			}
#endif		
			echo_to_sysfs();
		}

		snapshot_before = snapshot_end;
#ifdef FINGER_PRINT
		fp_snapshot_before = fp_snapshot_end;
#endif
	}
#ifdef DEBUG
	printk("kernel thread exit\n");	
#endif

	return 0;
}
#endif

#ifdef SUBSYS_DISK
static int kthread_work_disk(void *data)
{
	unsigned long metric;
#ifdef FINGER_PRINT
	struct fp_snapshot fp_ss;
	struct fingerprint fp, stored_fp;
#endif
	int not_cal = 1, first_cal = 1;
#ifdef FINGER_PRINT
	int i, found;
#endif
	
	snapshot_init(&snapshot_before);
#ifdef FINGER_PRINT
	get_disk_stats(&snapshot_before, &fp_snapshot_before);
#else
	get_disk_stats(&snapshot_before);
#endif

	allow_signal(SIGKILL);
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(msecs_to_jiffies(CONFIG_LIFE_TIME));	

		snapshot_init(&snapshot_end);

#ifdef FINGER_PRINT
		get_disk_stats(&snapshot_end, &fp_snapshot_end);
#else
		get_disk_stats(&snapshot_end);
#endif

#ifdef DISKOPS
		metric = disk_ops_calc(&snapshot_before, &snapshot_end);
#elif defined(DISKTHP)
		metric = disk_thoughput_calc(&snapshot_before, &snapshot_end);
#else
	        metric = disk_latency_calc(&snapshot_before, &snapshot_end); 
#endif
		
#ifdef DEBUG
		printk("metric = %lu\n", metric);		
#endif

#if defined(DISKOPS) || defined(DISKTHP)
                search_engine(metric, 1);
#else
                search_engine(metric, 0);
#endif

#ifdef FINGER_PRINT
		cal_fp_snapshot_diff(&fp_ss);
		cal_fp(&fp, &fp_ss);
#endif

		if (1 == first_cal) {
#ifdef DEBUG
			printk("FIRST CAL\n");
#endif

#ifdef FINGER_PRINT
			found = 0;
			for (i = 0; i <= perfcache.guard; i++) {
				if (1 == hit_cache(fp, perfcache.perfc[i].fp)) {
#ifdef DEBUG
					printk("FOUND PERVIOUS RESULTS\n");
        				printk("fp.concurrency=%d fp.rw_ratio=%d fp.avg_size=%lu fp.avg_tt=%lu fp.avg_dist=%lu\n", fp.concurrency, fp.rw_ratio, fp.avg_size, fp.avg_tt, fp.avg_dist);
        				printk("cfp.concurrency=%d cfp.rw_ratio=%d cfp.avg_size=%lu cfp.avg_tt=%lu cfp.avg_dist=%lu\n", perfcache.perfc[i].fp.concurrency, perfcache.perfc[i].fp.rw_ratio, perfcache.perfc[i].fp.avg_size, perfcache.perfc[i].fp.avg_tt, perfcache.perfc[i].fp.avg_dist);
#endif
					record = perfcache.perfc[i].rcd;
					found = 1;
					if (AUTO_COMPLETED == record.completed)
						not_cal = 2;
					else 
						not_cal = 1;
					break;	
				}
			}
			stored_fp = fp;
			if (0 == found) 
				printk("NOT FOUND PREVIOUS RESULTS, CONTINUE TO SEARCH\n");
#endif
	
			first_cal = 0;
			echo_to_sysfs();

		} else if (AUTO_COMPLETED == record.completed && 1 == not_cal) {

#ifdef FINGER_PRINT
			found = 0;
			for (i = 0; i <= perfcache.guard; i++) {
				if (1 == hit_cache(stored_fp, perfcache.perfc[i].fp) && AUTO_COMPLETED != perfcache.perfc[i].rcd.completed) {
					printk("FOUND PREVIOUS UNCOMPLETED RUN, REPLACE\n");
					perfcache.perfc[i].fp = stored_fp;
					perfcache.perfc[i].rcd = record;
					found = 1;
					break;	
				}
			}	
			
			if (0 == found && perfcache.guard < CACHE_SIZE - 1) {
				perfcache.guard ++;
				perfcache.perfc[perfcache.guard].fp = stored_fp;
                        	perfcache.perfc[perfcache.guard].rcd = record;
			}
			else printk("FOUND OR NO SLOT TO STORE\n");
#endif	
			not_cal = 0;
			echo_to_sysfs();

		} else if (AUTO_COMPLETED != record.completed) { 

#ifdef FINGER_PRINT
			if (kthread_should_stop()) {
				found = 0;				
				for (i = 0; i <= perfcache.guard; i++) {
					if (1 == hit_cache(stored_fp, perfcache.perfc[i].fp) && AUTO_COMPLETED != perfcache.perfc[i].rcd.completed) {
						printk("FOUND PREVIOUS UNCOMPLETED RUN, REPLACE\n");
						perfcache.perfc[i].fp = stored_fp;
						perfcache.perfc[i].rcd = record;
						found = 1;	
						break;
					}
				}
	
				if (0 == found && perfcache.guard < CACHE_SIZE - 1) {
					printk("STORE UNCOMPLETED RUN\n");
					perfcache.guard ++;
					perfcache.perfc[perfcache.guard].fp = stored_fp;
					perfcache.perfc[perfcache.guard].rcd = record;
				}
			}
#endif
			echo_to_sysfs();			
		}
		
        	snapshot_before = snapshot_end;	
#ifdef FINGER_PRINT	
		fp_snapshot_before = fp_snapshot_end;
#endif

#ifdef PERIOD 
		if (-1 ! = period_cnt) {
			period_cnt ++;
			if (0 == period_cnt % PERIOD) { 
				perf_recorder_init();
				not_cal = first_cal = 1;
				fp_snapshot_before = fp_snapshot_end;
			}			 
		}
#endif
	}

#ifdef DEBUG
	printk("kernel thread exit\n");	
#endif
	return 0;		
}
#endif

int fast_auto_proc_read(char *buf, char **start, off_t offset, int count, int *eof, void *data)
{

#ifdef DEBUG
	printk("offset = %d\n", offset);	
#endif	

	if (0 == offset) { 

		strncpy(buf, proc_buffer, MAX_PROCBUF_SIZE); 

		return  MAX_PROCBUF_SIZE; 
	}
	
	return 0;	
}

#ifdef SUBSYS_DISK
extern int start_to_tag;
#endif

int fast_auto_proc_write(struct file *file, const char *buf, int count, void *data)
{
	int errno;
	char *proc_buffer;	


	proc_buffer = kmalloc(count + 1, GFP_KERNEL);
	if (!proc_buffer)
		return -ENOMEM;

	if (copy_from_user(proc_buffer, buf, count)) {
		kfree(proc_buffer);
		return -EFAULT;
	}

	if (count && '\n' == proc_buffer[count-1])
		proc_buffer[count-1] = '\0';
	else 
		proc_buffer[count] = '\0';

	if (0 == strncmp("enable", proc_buffer, strlen(proc_buffer))) {
		if (OFF == mc.module_enable) {
			mc.module_enable = ON;
			mc.module_disable = OFF;
			perf_recorder_init();
			echo_to_sysfs();
			/*
			emergency_sync();
			echo_to_procfs('3');
			*/
#ifdef SUBSYS_DISK	
			start_to_tag = 1;
			IOCNT_THD = 200;
			clear_ids();
#endif
		
#ifdef SUBSYS_DISK
	        	task = kthread_run(kthread_work_disk, NULL, "TIMER");        
#elif defined (SUBSYS_CPU) || defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)
			task = kthread_run(kthread_work_cpu, NULL, "TIMER");
#endif
        		if (IS_ERR(task)) {
                		errno = PTR_ERR(task);
                		printk("cannot create kthread, errno = %d\n", errno);
                		return errno;
        		}
		}
	} else if (0 == strncmp("disable", proc_buffer, strlen(proc_buffer))) {	
		if (ON == mc.module_enable) {
			kthread_stop(task);
			mc.module_enable = mc.module_disable = OFF;
#ifdef SUBSYS_DISK
		 	clear_ids();	
			start_to_tag = 0;
#endif
		}
	} else {
		printk("Invalid argument\n");
	}

	kfree(proc_buffer);

	return count;
}

#if defined(SUBSYS_SPINLOCK) || defined(SUBSYS_MIXEDLOCK)
int spinlock_proc_write(struct file *file, const char *buf, int count, void *data)
{
        char *proc_buffer;

        proc_buffer = kmalloc(count + 1, GFP_KERNEL);
        if (!proc_buffer) 
                return -ENOMEM;

        if (copy_from_user(proc_buffer, buf, count)) {
                kfree(proc_buffer);
                return -EFAULT;
        }

        if (count && '\n' == proc_buffer[count-1])
                proc_buffer[count-1] = '\0';
        else
                proc_buffer[count] = '\0';

	val_tuner = convert(proc_buffer);
#ifdef DEBUG
	printk("val_tuner = %d\n", val_tuner);
#endif
        kfree(proc_buffer);

        return count;
}	

int mixedlock_proc_write(struct file *file, const char *buf, int count, void *data)
{
        char *proc_buffer;

        proc_buffer = kmalloc(count + 1, GFP_KERNEL);
        if (!proc_buffer)
                return -ENOMEM;

        if (copy_from_user(proc_buffer, buf, count)) {
                kfree(proc_buffer);
                return -EFAULT;
        }

        if (count && '\n' == proc_buffer[count-1])
                proc_buffer[count-1] = '\0';
        else
                proc_buffer[count] = '\0';

	if (0 == strncmp("TAS", proc_buffer, strlen(proc_buffer)))
		method_tuner = 0;
	else if (0 == strncmp("QUEUE", proc_buffer, strlen(proc_buffer)))
		method_tuner = 1;
	else if (0 == strncmp("TICKET", proc_buffer, strlen(proc_buffer)))
		method_tuner = 2;
#ifdef DEBUG
        printk("method_tuner = %d\n", method_tuner);
#endif
        kfree(proc_buffer);

        return count;
}
#endif

static __init int auto_init(void)
{
	int ret;
	struct proc_dir_entry *entry;	
#if defined(SUBSYS_SPINLOCK) || defined(SUBSYS_MIXEDLOCK)
	struct proc_dir_entry *spinlock_entry;
#ifdef SUBSYS_MIXEDLOCK
	struct proc_dir_entry *mixedlock_entry;
#endif
#endif
	
	global_init();
	ret = enhanced_spec_parser();
		
	if (ret < 0) 
		return ret;
	
#ifdef FILE_STORE
	cache_init();
#endif

	entry = create_proc_entry("fast_auto", 0600, NULL);
	if (!entry)	
		return -ENOMEM;

	entry->read_proc = fast_auto_proc_read;
	entry->write_proc = fast_auto_proc_write;

#if defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK)
	spinlock_entry = create_proc_entry("backoff_factor", 0600, NULL);
	if (!spinlock_entry) 
		return -ENOMEM;

	spinlock_entry->write_proc = spinlock_proc_write;
#ifdef SUBSYS_MIXEDLOCK
	mixedlock_entry = create_proc_entry("lock_selector", 0600, NULL);
	if (!mixedlock_entry)
		return -ENOMEM;

	mixedlock_entry->write_proc = mixedlock_proc_write;
#endif
#endif	
	 
	return 0;
}

#ifdef FILE_STORE
static unsigned long get_checksum(char *buf)
{
	unsigned long check_sum = 0UL;
	char *p = buf;
	while ('\0' != *p) {
		check_sum += (unsigned long)(*p);
		p ++; 
	}

	return check_sum;
}
#endif

static __exit void auto_exit(void)
{

#ifdef FILE_STORE
	struct file *file;
	int ret, i, j, size;
	unsigned long check_sum;
	char tmpbuf[(4*MAXLEN)], *p = tmpbuf, *specbuf;	
#endif

	if (ON == mc.module_enable && OFF == mc.module_disable) {
		kthread_stop(task);
#ifdef SUBSYS_DISK
		clear_ids();
		start_to_tag = 0;
#endif
	}	

	remove_proc_entry("fast_auto", NULL);

#if defined (SUBSYS_SPINLOCK) || defined (SUBSYS_MIXEDLOCK) 
	remove_proc_entry("backoff_factor", NULL);
#ifdef SUBSYS_MIXEDLOCK
	remove_proc_entry("lock_selector", NULL);
#endif
#endif	

#ifdef FILE_STORE
	specbuf = read_into_buffer("/etc/SPEC/spec", &size); 	
	if (NULL == specbuf) {
		printk("Error when reading spec into buffer\n");
		return;
	}

	check_sum = get_checksum(specbuf);
	if (0 == check_sum) {
		printk("Empty spec file\n");
		return;
	}

#ifdef DEBUG
	printk("check sum = %lu\n", check_sum);
#endif
	free_buffer(specbuf, size);	

	memset(tmpbuf, '\0', sizeof(tmpbuf));
	p += sprintf(p, "%lu\n", check_sum); 
	for (i=0; i < vector.dim_guard; i++) {
		p += sprintf(p, "%d\t", backup_rcd.conf[i]); 
	}
	p += sprintf(p, "\n");
	for (i=0; i < vector.dim_guard; i++) {
		p += sprintf(p, "%d\t", backup_rcd.bestconf[i]);
	}
	p += sprintf(p, "\n");
	p += sprintf(p, "%lu\t", backup_rcd.best);
	p += sprintf(p, "%d\t", backup_rcd.completed);
	p += sprintf(p, "%d\n", backup_rcd.dim_loc);
	p += sprintf(p, "%d\n", backup_rcd.loc.enumloc);
	p += sprintf(p, "%d\n", backup_rcd.to_value);

#ifdef FINGER_PRINT	
	p += sprintf(p, "%d\n", perfcache.guard);
	for (i = 0; i<= perfcache.guard; i++) {
		p += sprintf(p, "%d\t%d\t%lu\t%lu\t%lu\n", perfcache.perfc[i].fp.concurrency, perfcache.perfc[i].fp.rw_ratio, perfcache.perfc[i].fp.avg_size, perfcache.perfc[i].fp.avg_tt, perfcache.perfc[i].fp.avg_dist);
	
		for (j = 0; j < vector.dim_guard; j++) {
			p += sprintf(p, "%d\t", perfcache.perfc[i].rcd.conf[j]);
		} 
		p += sprintf(p, "\n");

		for (j = 0; j < vector.dim_guard; j++) {
			p += sprintf(p, "%d\t", perfcache.perfc[i].rcd.bestconf[j]);
		}
		p += sprintf(p, "\n");

		p += sprintf(p, "%lu\t", perfcache.perfc[i].rcd.best);
		p += sprintf(p, "%d\t", perfcache.perfc[i].rcd.completed);
		p += sprintf(p, "%d\n", perfcache.perfc[i].rcd.dim_loc);
		p += sprintf(p, "%d\n", perfcache.perfc[i].rcd.loc.enumloc);
		p += sprintf(p, "%d\n", perfcache.perfc[i].rcd.to_value);
	}
#endif
	
        file = open_file("/etc/SPEC/cache", O_RDWR | O_CREAT, 0777);
        if (IS_ERR(file)) {
                ret = PTR_ERR(file);
                printk("Cannot open /etc/SPEC/cache for writing, error=%d\n", ret);
                return;
        }

	ret = kernel_write(file, 0, tmpbuf, (4*MAXLEN));
	if (ret < 0)
		printk("error when writing to /etc/SPEC/cache\n");	
	close_file(file);		 

#endif

	return;
}

MODULE_AUTHOR("YAN CUI <ccuiyyan@gmail.com>");
MODULE_LICENSE("GPL");
	
module_init(auto_init);
module_exit(auto_exit);
