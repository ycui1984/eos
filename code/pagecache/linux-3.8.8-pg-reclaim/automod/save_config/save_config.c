#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <asm/uaccess.h>
#include <linux/genhd.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>

#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/mmzone.h>

#define SC_AUTHOR "Petre Lukarov <pbl2108@columbia.edu>"
#define SC_LICENSE "GPL"
#define SC_DESC   "Module that saves the sysfs changes into config file to persist them across reboots."

#define SYSFS_CONF "/etc/sysfs.conf"

static struct file *open_file(char const *file_name, int flags, int mode)
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

static int kernel_write(struct file *file, unsigned long offset,
			const char *addr, unsigned long count)
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

static int echo_to_config(char *buf, int count) {
	struct file *file;
	int ret;

	file = open_file(SYSFS_CONF, O_RDWR|O_CREAT, 0644);

	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		printk("cannot open '%s', error=%d\n", SYSFS_CONF, ret);
		return -1;
	}	

	ret = kernel_write(file, 0, buf, count);
	if (ret < 0) {
		printk("cannot write to '%s'\n", SYSFS_CONF);
		return -1;
	}

	close_file(file);

	return 0;
}

/*static char *read_into_buffer(char *file_name, int *size)
{
	int ret, file_size, data_read = 0;
	struct file *file;
	void *buffer;
	
	file = open_file(file_name, O_RDONLY, 00777);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		printk("Cannot open %s for reading, error = %d\n", file_name,
			ret);
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
		
		ret = kernel_read(file, data_read, buffer + data_read,
				  file_size - data_read);
			
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
}*/

static __init int save_config_init(void)
{
	int ret = 0;
	printk(KERN_INFO "init(): save_config module init.\n");
	return ret;
}

static __exit void save_config_exit(void)
{
	printk(KERN_INFO "exit(): save_config ************************\n");
	return;
}

MODULE_AUTHOR(SC_AUTHOR);
MODULE_LICENSE(SC_LICENSE);
MODULE_DESCRIPTION(SC_DESC);

module_init(save_config_init);
module_exit(save_config_exit);
