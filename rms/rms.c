#define LINUX

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/rcupdate.h>
#include <linux/pid.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/compiler.h>
#include "rms.h"

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_entry;

static LIST_HEAD(rms_task_list);
struct rms_task_struct {
	struct task_struct *task;
	struct timer_list wakeup_timer;
	struct list_head list;
	pid_t pid;
	unsigned long period_ms;
	unsigned long runtime_ms;
	unsigned long deadline_jiff;
	enum task_state state;
};
static struct kmem_cache *rms_task_struct_cache;
static DEFINE_MUTEX(rms_task_list_lock);

static struct task_struct *dispatch_thread;
static struct rms_task_struct *curr_rms_task;
static DEFINE_MUTEX(curr_task_ptr_lock);

/* 
 * Retrieves the READY task with the highest priority 
 * (i.e., the READY task that has the shortest period)
 * in task list.
 */
static struct rms_task_struct *highest_prio_task(void)
{	
	struct rms_task_struct *hgst_prio;
	struct rms_task_struct *rms_tsk;
	unsigned long shst_period;
	
	hgst_prio = NULL;
	shst_period = 0;
	mutex_lock(&rms_task_list_lock);
	list_for_each_entry(rms_tsk, &rms_task_list, list)
		if (rms_tsk->state == READY &&
			(hgst_prio == NULL || 
				rms_tsk->period_ms < shst_period)) {
			hgst_prio = rms_tsk;
			shst_period = rms_tsk->period_ms;
		}
	mutex_unlock(&rms_task_list_lock);
	return hgst_prio;
}

/* 
 * The function that the dispatching thread will run. 
 * The main task of this function is to trigger context
 * switches and put the dispatching thread sleep the
 * rest of the time.
 */
static int dispatch_thread_fn(void *data)
{	
	struct rms_task_struct *nxt_tsk;
	struct sched_attr attr;

	while (1) {
		/* 
		 * Put the dispatching thread to sleep, as we want it
		 * to be sleeping when there is no scheduling work to do.
		 */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		/* After waking up, first check if it is waked up to exit */
		if (kthread_should_stop())
			return 0;

		/* Trigger context switch */
		nxt_tsk = highest_prio_task();
		if (nxt_tsk) {
			/* Schedule the next READY job of highest prority */
			nxt_tsk->state = RUNNING;
			wake_up_process(nxt_tsk->task);
			attr.sched_policy = SCHED_FIFO;	
			/*
			 * sched_priority sets rt_priority,
			 * i.e., the rt_priority will be 99.
			 */
			attr.sched_priority = 99;
			sched_setattr_nocheck(nxt_tsk->task, &attr);
		}
		mutex_lock(&curr_task_ptr_lock);
		if (curr_rms_task) {
			/* Preempt currently running task */
			curr_rms_task->state = READY;
			attr.sched_policy = SCHED_FIFO;
			/* None-RT tasks don't use sched_priority */
			attr.sched_priority = 0; 
			sched_setattr_nocheck(curr_rms_task->task, &attr);
		}
		curr_rms_task = nxt_tsk ? nxt_tsk : NULL;
		mutex_unlock(&curr_task_ptr_lock);
	}
}

static ssize_t usr_read(struct file *file,
						char __user *buffer,
						size_t count, loff_t *off)
{
	int copied;
	char *kbuf;
	struct rms_task_struct *rms_tsk;

	copied = 0;
	kbuf = (char *)kmalloc(count, GFP_KERNEL);
	if (kbuf == NULL) {
		printk(KERN_ALERT "error: kmalloc: no memory available\n");
		return copied;
	}
	mutex_lock(&rms_task_list_lock);
	list_for_each_entry(rms_tsk, &rms_task_list, list)
		copied += sprintf(kbuf + copied, "%d: %lu, %lu\n", 
			rms_tsk->pid, rms_tsk->period_ms, rms_tsk->runtime_ms);
	mutex_unlock(&rms_task_list_lock);
	kbuf[copied] = '\0';
	if (copy_to_user(buffer, kbuf, copied)) {
		printk(KERN_ALERT "error: copy_to_user failed\n");
	}
	kfree(kbuf);

	#ifdef DEBUG
	printk(KERN_INFO "USER READ\n");
	#endif
    
	if (*off > 0) /* EOF condition */
		return 0; /* End of file */
	*off += copied;
	return copied;
}

/* 
 * Admits task (i.e., returns 1) only if the following
 * equation is satisfied. 
 * 
 * \sum_{i\in T} C_i/P_i <= 0.693
 *
 * where T is the set of all tasks including the already
 * registered tasks and the task to be admitted, C_i is
 * the processing time per period P_i for the i-th task.
 *
 * Fixed point arithmetic is used to perform the test. 
 */
static int admit_task(unsigned long period_ms,
					  unsigned long runtime_ms)
{
	/* 
	 * From the results in the Sha et al.'s Generalized
	 * RMS Theory: A framweork for developing RTS paper, 
	 * one can conclude that if \sum_{i\in T} C_i/P_i is 
	 * less than or equal to some number close enough to 
	 * 0.62 on the right hand side, then the RMS scheduler
	 * will always meet the deadlines. We choose 0.693 to
	 * be the bound. fr stands for the fractional part. 
	 */
	static const unsigned long bnd_fr = 693;
	struct rms_task_struct *rms_tsk;
	unsigned long sum_ra;

	sum_ra = (runtime_ms << SHIFT_AMOUNT) / period_ms;
	mutex_lock(&rms_task_list_lock);
	list_for_each_entry(rms_tsk, &rms_task_list, list)
		sum_ra += (rms_tsk->runtime_ms << SHIFT_AMOUNT) /
				  rms_tsk->period_ms;
	mutex_unlock(&rms_task_list_lock);
	/* Compare the intergral parts */
	if (unlikely((sum_ra >> SHIFT_AMOUNT) > 0)) 
		return 0;
	/* Compare the fractional parts */
	if (unlikely((sum_ra & SHIFT_MASK) * 1000 / (1 << SHIFT_AMOUNT) > bnd_fr))
		return 0;
	return 1;
}

static struct task_struct *find_task_by_pid(int nr)
{
	struct task_struct *task;

	rcu_read_lock();
	task = pid_task(find_vpid(nr), PIDTYPE_PID);
	rcu_read_unlock();
	return task;
}

/* 
 * Wakeup timer interrupt handler.
 * Changes the state of the task containing the timer
 * to READY and wake the dispatching thread up to
 * trigger context switch.
 */
static void _wakeup_timer_fn(struct timer_list *tl) 
{
	struct rms_task_struct *rms_tsk;

	rms_tsk = from_timer(rms_tsk, tl, wakeup_timer);
	rms_tsk->state = READY;
	wake_up_process(dispatch_thread);
}

static void register_task(char *msg)
{
	struct rms_task_struct *rms_tsk;

	rms_tsk = (struct rms_task_struct*)
			kmem_cache_alloc(rms_task_struct_cache, GFP_KERNEL);
	sscanf(strsep(&msg, ","), "%d", &rms_tsk->pid);
	sscanf(strsep(&msg, ","), "%lu", &rms_tsk->period_ms);
	sscanf(strsep(&msg, ","), "%lu", &rms_tsk->runtime_ms);

	if (!admit_task(rms_tsk->period_ms, rms_tsk->runtime_ms))
		return;

	rms_tsk->state = SLEEPING;
	rms_tsk->deadline_jiff = 0;

	rms_tsk->task = find_task_by_pid(rms_tsk->pid);
	timer_setup(&rms_tsk->wakeup_timer, _wakeup_timer_fn, 0);
	INIT_LIST_HEAD(&rms_tsk->list);
	mutex_lock(&rms_task_list_lock);
	list_add(&rms_tsk->list, &rms_task_list);
	mutex_unlock(&rms_task_list_lock);
}

/* Deschedule the task that sent the YIELD message */
static void deschedule_task(char *msg) 
{
	struct rms_task_struct *rms_tsk;
	int pid;

	sscanf(msg, "%d", &pid);
	rms_tsk = NULL;
	mutex_lock(&rms_task_list_lock);
	list_for_each_entry(rms_tsk, &rms_task_list, list)
		if (rms_tsk->pid == pid)
			break;
	mutex_unlock(&rms_task_list_lock);
	if (rms_tsk == NULL) {
		/* Process requesting not registered */
		return;
	}
	if (rms_tsk->deadline_jiff == 0) {
		/* The task is just newly registered */
		rms_tsk->deadline_jiff = jiffies;
	}
	rms_tsk->deadline_jiff += msecs_to_jiffies(rms_tsk->period_ms);
	if (rms_tsk->deadline_jiff < jiffies) {
		/* 
		 * The next period has already started.
		 * Do nothing.
		 */
		return;
	}
	rms_tsk->state = SLEEPING;
	mutex_lock(&curr_task_ptr_lock);
	curr_rms_task = NULL;
	mutex_unlock(&curr_task_ptr_lock);
	mod_timer(&rms_tsk->wakeup_timer, rms_tsk->deadline_jiff);
	wake_up_process(dispatch_thread);
	set_task_state(rms_tsk->task, TASK_UNINTERRUPTIBLE);
}

static void deregister_task(char *msg)
{
	struct rms_task_struct *rms_tsk, *temp;
	int pid;

	sscanf(msg, "%d", &pid);
	mutex_lock(&rms_task_list_lock); 
	list_for_each_entry_safe(rms_tsk, temp, &rms_task_list, list)
		if (rms_tsk->pid == pid) {
			list_del(&rms_tsk->list);
			del_timer_sync(&rms_tsk->wakeup_timer);
			kmem_cache_free(rms_task_struct_cache, rms_tsk);
		}
	mutex_unlock(&rms_task_list_lock);
}

static ssize_t usr_write(struct file *file,
						 const char __user *buffer,
						 size_t count, loff_t *off)
{
	char *kbuf;

	/* count counts the null-terminator */
	kbuf = kmalloc(count, GFP_KERNEL); 
	if (kbuf == NULL) {
		printk(KERN_ALERT "error: kmalloc: no memory available\n");
		return -ENOMEM;
	}
	if (copy_from_user(kbuf, buffer, count)) {
		printk(KERN_ALERT "error: copy_from_user failed\n");
	}
	/* copy_from_user() doesn't copy the null-terminator */
	kbuf[count-1] = '\0'; 
	switch (kbuf[0]) {
	case REGISTERATION:
		register_task(kbuf + 3);
		break;
	case YIELD:
		/* the YIELD handler */
		deschedule_task(kbuf+3); 
		break;
	case DEREGISTRATION:
		deregister_task(kbuf+3);
		break;
	default:
		printk(KERN_ALERT "error: write: invalid message type\n");
	}
	kfree(kbuf);

	#ifdef DEBUG
	printk(KERN_INFO "USER WRITED\n");
	#endif

	return count;
}

/* Use proc_ops instead of file_operations on version >= 5.6 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops rms_file = {
	.proc_read = usr_read,
	.proc_write = usr_write,
};
#else
static const struct file_operations rms_file = {
	.owner = THIS_MODULE,
	.read = usr_read,
	.write = usr_write,
};
#endif

int __init rms_init(void)
{
	#ifdef DEBUG
	printk(KERN_INFO "RMS MODULE LOADING\n");
	#endif

	/* Create the proc filesystem entries: rms/ and rms/status */
	proc_dir = proc_mkdir(DIRECTORY, NULL);
	if (proc_dir == NULL) {
		printk(KERN_ALERT "error: proc_mkdir failed\n");
		return -ENOMEM;
	}
	proc_entry = proc_create(FILENAME, 0666, proc_dir, &rms_file);
	if (proc_entry == NULL) {
		printk(KERN_ALERT "error: proc_create failed\n");
		return -ENOMEM;
	}
	/* Set up the cache for slab allocator of rms_task_struct */
	rms_task_struct_cache = kmem_cache_create("RMS Slab Alloc Cache", 
		sizeof(struct rms_task_struct), 0, SLAB_HWCACHE_ALIGN, NULL); 
	if (rms_task_struct_cache == NULL) {
		printk(KERN_ALERT "error: kmem_cache_create failed\n");
		return -ENOMEM;
	}
	/* Create and wake up a thread that triggers context switches */
	dispatch_thread = kthread_run(dispatch_thread_fn, NULL, 
								  "RMS Dispatching Thread");
	if (IS_ERR(dispatch_thread)) {
		printk(KERN_ALERT "error: kthread_create failed\n");
		return -ENOMEM;
	}

	#ifdef DEBUG
	printk(KERN_INFO "RMS MODULE LOADED\n");
	#endif
	
	return 0;
}

void __exit rms_exit(void)
{
	struct rms_task_struct *rms_tsk, *temp;

	#ifdef DEBUG
	printk(KERN_INFO "RMS MODULE UNLOADING\n");
	#endif

	/* Remove the proc filesystem entries created in init */
	remove_proc_entry(FILENAME, proc_dir);
	remove_proc_entry(DIRECTORY, NULL);
	/* Free all of the objects in the task list */
	list_for_each_entry_safe(rms_tsk, temp, &rms_task_list, list) {
			list_del(&rms_tsk->list);
			del_timer_sync(&rms_tsk->wakeup_timer);
			kmem_cache_free(rms_task_struct_cache, rms_tsk);
	}
	/* Destroy the cache set up for slab allocator */
	kmem_cache_destroy(rms_task_struct_cache);
	/* Stop the dispatching function */
	kthread_stop(dispatch_thread);

	#ifdef DEBUG
	printk(KERN_INFO "RMS MODULE UNLOADED\n");
	#endif
}

module_init(rms_init);
module_exit(rms_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TAKAOTSUTOMU");
MODULE_DESCRIPTION("Rate-Monotonic Scheulder module that schedules real-time tasks on top of Linux Scheduler.");