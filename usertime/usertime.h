#ifndef __USERTIME_H__
#define __USERTIME_H__

#include <linux/pid.h>
#include <linux/kthread.h>

#define FILENAME "status"
#define DIRECTORY "usrt"
#define INTERVAL 5000
#define find_task_by_pid(nr) \
			pid_task(find_vpid(nr), PIDTYPE_PID)
#endif