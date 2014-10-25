#include <linux/linkage.h>
#include <linux/uaccess.h>
#include <linux/module.h>

#define GET_VAL_TUNNER		1
#define SET_VAL_TUNNER		2
#define GET_METHOD_TUNNER	3
#define SET_METHOD_TUNNER	4	

/* backoff lock tuner */
volatile int val_tuner = 0;
EXPORT_SYMBOL(val_tuner);

/* reactive lock tuner */
volatile int method_tuner = 0;
EXPORT_SYMBOL(method_tuner);

asmlinkage long sys_tuner(int id, int value)
{
	if (id == GET_VAL_TUNNER)
		return (long)val_tuner;
	
	if (id == GET_METHOD_TUNNER)
		return (long)method_tuner;

	if (id == SET_VAL_TUNNER) {
		if (value < 0) {
			printk("val tunner should be >= 0\n");
			return -1;
		}
		val_tuner = value;
		return 0;
	}

	if (id == SET_METHOD_TUNNER) {
		if (value < 0) {
			printk("method tunner shoule be >= 0\n");
			return -1;
		}
		method_tuner = value;
		return 0;	
	}
	
	return -1;		
}
