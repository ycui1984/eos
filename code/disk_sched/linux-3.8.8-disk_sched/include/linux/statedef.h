#ifndef __STATEDEF__
#define __STATEDEF__

#define SUBSYS_DISK		/* disk scheduling */
//#define SUBSYS_LOCK   	/* mutex lock */
//#define SUBSYS_SPINLOCK	/* backoff ticket lock */
//#define SUBSYS_CPU		/* CPU scheduling */
//#define SUBSYS_MIXEDLOCK	/* reactive lock */

#define EVOLVE_COMPLETED	2
#define EVOLVE_INPROCESS	1

#define NOTCACHED		2
#define CACHED			1

#define MAXDIM                  50
#define AUTO_COMPLETED          1
#define EXCEED_SPIN_THRESHOLD   2

#define START_SIGNALED		1
#define COMMON_RUN		2
#define MAXCORES		4
#define CACHELINE		64

struct VALUELOC {
        int enumloc;
};

struct RECORDER {
        int conf[MAXDIM];
        int bestconf[MAXDIM];
        unsigned long best;
        int completed;
        int dim_loc;
        struct VALUELOC loc;
        int to_value;
};
#endif
