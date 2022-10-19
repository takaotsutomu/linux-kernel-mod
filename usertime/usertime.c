#define LINUX

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/kstrtox.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include "usertime.h"

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_entry;

static LIST_HEAD(cput_list);
struct cput_entry {
	unsigned int pid;
	unsigned long time;
	struct list_head list;
};

static DEFINE_MUTEX(cput_list_lock);
static struct timer_list ktimer;
static struct workqueue_struct *workqueue;
static struct work_struct work;

/* Helper function that help update the cputimes */
int get_cpu_use(int pid, unsigned long *cpu_use)
{
	struct task_struct *task;

	rcu_read_lock();
	task = find_task_by_pid(pid);
	if (task != NULL) {
		*cpu_use = task->utime;
		rcu_read_unlock();
		return 0;
	} else {
		rcu_read_unlock();
		return -1;
	}
}

/* 
 * Work function 
 * (Botton-Half of the Two-Halves interrupt handler design) 
 */
static void update_cputimes(struct work_struct *work)
{
   struct cput_entry *entry, *temp;

	mutex_lock(&cput_list_lock); 
	list_for_each_entry_safe(entry, temp, &cput_list, list)
		if (get_cpu_use(entry->pid, &entry->time)) {
			list_del(&entry->list);
			kfree(entry);
		}
	mutex_unlock(&cput_list_lock);
	mod_timer(&ktimer, jiffies + msecs_to_jiffies(INTERVAL));
}

/* 
 * Timer interrupt handler 
 * (the Top-Half of the Two-Halves interrupt handler design) 
 */
static void schedule_cput_upds(struct timer_list *timer)
{
	/* Skip scheduling work if there are no registered processes */
	if (list_empty(&cput_list)) {
		mod_timer(&ktimer, jiffies + msecs_to_jiffies(INTERVAL));
		return;
	}
	/* Schedule update_cputimes(), the work function w/ the workqueue */
	if (!queue_work(workqueue, &work)) {
		printk(KERN_ALERT "error: queue_work failed\n");
	}
}

static ssize_t usr_read(struct file *file,
								char __user *buffer, 
								size_t count, loff_t *off)
{
	int copied;
	char *kbuf;
	struct cput_entry *entry;

	copied = 0;
	kbuf = (char *)kmalloc(count, GFP_KERNEL);
	if (kbuf == NULL) {
		printk(KERN_ALERT "error: kmalloc: no memory available\n");
		return copied;
	}
	mutex_lock(&cput_list_lock);
	list_for_each_entry(entry, &cput_list, list)
		copied += sprintf(kbuf + copied, "%u: %u\n", 
			entry->pid, jiffies_to_msecs(clock_t_to_jiffies(entry->time)));
	mutex_unlock(&cput_list_lock);
	kbuf[copied] = '\0';
	if (copy_to_user(buffer, kbuf, copied)) {
		printk(KERN_ALERT "error: copy_to_user failed\n");
	}
	kfree(kbuf);

	#ifdef DEBUG
	printk(KERN_INFO "USER READ\n");
	#endif
     
	if (*off > 0) /* EOF condition */
		return 0;  /* End of file */
	*off += copied;
	return copied;
}

static ssize_t usr_write(struct file *file,
								 const char __user *buffer,
								 size_t count, loff_t *off)
{
	int pid;
	int ret;
	struct cput_entry *cput;

	if ((ret = kstrtoint_from_user(buffer, count, 10, &pid))) {
		switch (-ret) {
		case ERANGE:
			printk(KERN_ALERT "error: kstrtoint: overflow\n");
			break;
		case EINVAL:
			printk(KERN_ALERT "error: kstrtoint: parsing error\n");
			break;
		}
		return -EIO;
	}
	cput = kmalloc(sizeof(struct cput_entry), GFP_KERNEL);
	if (cput == NULL) {
		printk(KERN_ALERT "error: kmalloc: no memory available\n");
		return -ENOMEM;
	}
	cput->pid = pid;
	cput->time = 0;
	INIT_LIST_HEAD(&cput->list);
	mutex_lock(&cput_list_lock);
	list_add(&cput->list, &cput_list);
	mutex_unlock(&cput_list_lock);

	#ifdef DEBUG
	printk(KERN_INFO "USER WRITED\n");
	#endif

	return count;
}

/* Use proc_ops instead of file_operations on version >= 5.6 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops usrt_file = {
	.proc_read = usr_read,
	.proc_write = usr_write,
};
#else
static const struct file_operations usrt_file = {
	.owner = THIS_MODULE,
	.read = usr_read,
	.write = usr_write,
};
#endif

int __init usrt_init(void)
{
	#ifdef DEBUG
	printk(KERN_INFO "USRT MODULE LOADING\n");
	#endif

	/* Create the proc filesystem entries: usrt/ and usrt/status */
	proc_dir = proc_mkdir(DIRECTORY, NULL);
	if (proc_dir == NULL) {
		printk(KERN_ALERT "error: proc_mkdir failed\n");
		return -ENOMEM;
	}
	proc_entry = proc_create(FILENAME, 0666, proc_dir, &usrt_file);
	if (proc_entry == NULL) {
		printk(KERN_ALERT "error: proc_create failed\n");
		return -ENOMEM;
	}
	/* Setup a kernel timer that wakes up every 5 secs */
	timer_setup(&ktimer, schedule_cput_upds, 0);
	mod_timer(&ktimer, jiffies + msecs_to_jiffies(INTERVAL));
	/* 
	 * Create a workqueue for work scheduling and initialize 
	 * work to schedule with the work function defined above. 
	 */
	workqueue = create_workqueue("workqueue");
	if (workqueue == NULL) {
		printk(KERN_ALERT "error: create_workqueue failed\n");
		return -ENOMEM;
	}
	INIT_WORK(&work, update_cputimes);

	#ifdef DEBUG
	printk(KERN_INFO "USRT MODULE LOADED\n");
	#endif
	
	return 0;
}

void __exit usrt_exit(void)
{
	#ifdef DEBUG
	printk(KERN_INFO "USRT MODULE UNLOADING\n");
	#endif

	/* Remove the proc filesystem entries created in init */
	remove_proc_entry(FILENAME, proc_dir);
	remove_proc_entry(DIRECTORY, NULL);
	/* 
	 * Release the timer after the remaining handler finishes 
	 * its execution.
	 */
	del_timer_sync(&ktimer);
	/* 
	 * Wait until all pending works in the workqueue finish 
    * and destroy the workqueue.
    */
	flush_workqueue(workqueue);
	destroy_workqueue(workqueue);

	#ifdef DEBUG
	printk(KERN_INFO "USRT MODULE UNLOADED\n");
	#endif
}

module_init(usrt_init);
module_exit(usrt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TAKAOTSUTOMU");
MODULE_DESCRIPTION("Measures the CPU time of processes regiested with this module.");