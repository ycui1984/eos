#include <linux/module.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <asm/atomic.h>

extern void _init_lock(struct lock *lock);
extern enum release_mode _acquire_mixed_lock(struct lock *lock, volatile struct qnode* node);
extern void _release_mixed_lock(struct lock *lock, volatile struct qnode* node, enum release_mode mode);

unsigned long long nan_clock(void) 
{
   return (unsigned long long)jiffies * (NSEC_PER_SEC /HZ); 
} 

MODULE_LICENSE("Dual BSD/GPL"); 

#define NM_THREAD 3
#define NM_COUNT (2*1024*1024*1024) 
#define NM_LOOP	2  
#define NM_LOCKTYPE 1

u64 result[NM_LOCKTYPE][NM_THREAD+1];

unsigned int nm_type = 0;

struct task_struct * jack[NM_THREAD];

volatile unsigned int flag = 1;
volatile unsigned int stop_flag = 0;
atomic_t working_jack = {0};

u64 start_nansecond = 0;
u64 end_nansecond =0;


struct ticket_lock_counter{
  struct lock ml;
  unsigned int counter;
  struct task_struct * boss;
}c1;



void init_lock_wrapper(void)
{
  flag = 1;
  stop_flag = 0;
  atomic_set(&working_jack,0);
  init_lock(&c1.ml);
  c1.counter = 0;
  c1.boss = current;
}

static void kill_all(void)
{
  unsigned int i;
  for(i=0;i<NM_THREAD;i++){
    if((jack[i])&&(jack[i]!=current)){
      kthread_stop(jack[i]);
      jack[i] = NULL;
    }
  }
}

int hungry_ticket_jack(void *data)
{
  enum release_mode mode;
  volatile struct qnode node;	
#ifdef HELLO_DEBUG
  unsigned int jack_id = atomic_inc_return(&working_jack);
#else
  atomic_inc(&working_jack);
#endif
  
#ifdef HELLO_DEBUG
  printk("I am jack!NO %d\n",jack_id);
#endif
  while(flag){
    yield();
  }
  while(1){
    mode = acquire_mixed_lock(&c1.ml, &node);
#ifdef HELLO_DEBUG
    printk("%d got lock\n",jack_id);
#endif
    if(unlikely(c1.counter > NM_COUNT)){
      
      if(!stop_flag){
	end_nansecond = nan_clock();
	stop_flag = 1;
	flag = 1;
	wake_up_process(c1.boss);
#ifdef HELLO_DEBUG
	printk("%d firt counte %d\n",jack_id,c1.counter);
#endif
	release_mixed_lock(&c1.ml, &node, mode);
	goto out;
      }
      /*I am not the first guy,pity*/
      else{
	release_mixed_lock(&c1.ml, &node, mode);
	goto out;
      }
    }
    c1.counter++;
    /*          for(i=0;i<100;i++)
		dummy_counter++;*/

#ifdef HELLO_DEBUG
    printk("%d,relase\n",jack_id);
#endif
    release_mixed_lock(&c1.ml, &node, mode);


  }
 out:
  atomic_dec(&working_jack);
#ifdef HELLO_DEBUG
  printk("%d,I don't work anymore\n",jack_id);
#endif
  /* 	
  while(1){
    if(kthread_should_stop()){
#ifdef HELLO_DEBUG
      printk("Jack quit.NO %d\n",jack_id);
#endif
      return 0;
    }
    wake_up_process(c1.boss);
    yield();
  }
  */ 	
}


int boss(void *data)
{
  unsigned int i,j,k;
    
  /*Type 1, kernel ticket lock*/
  /*Type 2, MCS lock*/
  /*Type 3, arch lock*/
  for(nm_type=0;nm_type<NM_LOCKTYPE;nm_type++){

  for(j=0;j<NM_LOOP;j++){
    for(k=NM_THREAD;k<=NM_THREAD;k++){
      /*Init jack[] and c1*/
      for(i=0;i<NM_THREAD;i++)
	jack[i] = NULL;
      init_lock_wrapper();
      for(i=0;i<k;i++){
	jack[i] = kthread_run(hungry_ticket_jack,NULL,"ticket");
	if(IS_ERR(jack[i])){
	  printk("Error: When create hungry_ticket_jack %d\n",i);
	  jack[i] = NULL;
	  goto out;
	}
      }
  
      local_irq_disable();
      current->state = TASK_INTERRUPTIBLE;
      start_nansecond = nan_clock();
      flag = 0;
      local_irq_enable();
      schedule();
    /*End of this loop*/
      if(!flag){
	printk("Why wake up me, may be wrong\n");
	goto out;
      }
      //      printk("type %d,Loop%d,thread %d,counte %d: %llu,%llu,%llu\n",nm_type,j,k,c1.counter,start_nansecond,end_nansecond,end_nansecond-start_nansecond);           
#ifdef HELLO_DEBUG
      printk("[%d,%d,%d,%d,%llu]\n",nm_type,j,k,c1.counter,end_nansecond-start_nansecond);
#endif
      result[nm_type][k] = end_nansecond-start_nansecond;      
      /*Wait for all working jacks quit*/
      while(atomic_read(&working_jack))
	yield();
	
      kill_all();

    }
  }
  }

  printk("BOSS say: Dismiss\n");
  
  for(i=0;i<NM_LOCKTYPE;i++){
    for(j=1;j<=NM_THREAD;j++){
      printk(KERN_ALERT"[%d,%d,%llu]\n",i,j,result[i][j]);
    }
  }
  do_exit(0);

 out:
  kill_all();
  do_exit(0);
}
  
  
    

static int hello_init(void)
{
  printk("hello world\n");
  /*Create the boss!*/
  if(!kthread_run(boss,NULL,"BOSS")){
    printk("Error, can not create boss\n");
  }
  return 0;
}


static void hello_exit(void)
{
  printk("see u \n");  
}

module_init(hello_init);
module_exit(hello_exit);

