# Linux Real-Time Rate Monotonic Scheduler Module
## Overview
Several systems we use everyday are time-critical; they have requirements in terms of response time (e.g. delay and jitter) and predictability for safety or better user experience. For instance, a surveillance system recording video in a restricted area may be designed to capture a video frame every 30 milliseconds. If the capture is not properly scheduled, the video quality will be severely degraded. 

**This Linux kernel module implements a Real-Time Rate Monotonic Scheduler (RMS) for periodic tasks on a single CPU core.** A periodic task, as defined by [Liu and Layland](https://dl.acm.org/doi/10.1145/321738.321743), is a task in which a job that requires processing time C is released after every predefined period P, and must be completed before the beginning of the next period, referred to as deadline D. As in the case of the capture of a video frame in a surveillance system, C and P are usually known parameters for real-time jobs. The RMS scheduler is a static priority scheduler, in which priorities are determined based on the period of the job: the shorter the period, the higher the priority. It is preemptive and will always preempt a task with lower priority for the higher one until the higher one's proccessing time is used. 

The user application must interect with the RMS module by sending following messages via the Proc filesystem entry `/proc/rms/status` the RMS module sets up upon its installation. 
* REGISTERATION: Notify the RMS module that an application will use its scheduling service. The massage are strings with the following format:
    ```
    R, <pid>, <period>, <processing time>
    ```
    Note that "R" is a literal 'R' that denotes that this is a registration message.
* YIELD: Notify the RMS module that the application has finished its period. After sending a yield message, the application will block until the RMS scheduler wakes it up in the beginning of the next period. Yied massages are strings with the following format:
    ```
    Y, <pid>
    ```
* DEREGISTRERATION: Notify the RMS module that the application has finished using its scheduling serveice. This message has the follofwing format:
    ```
    D, <pid>
    ```

The RMS module will only register a new periodic application if the application passes the admission control with its parameters. The details of admission control is discribed in the comments of `admit_task` function in the source code. The RMS module decides if the new application can be scheduled along with the already admitted application without any deadlines of jobs to be missed for all regiestered application, using the results from the [Generalized RMS Theory](https://ieeexplore.ieee.org/document/259427) paper by Sha et al.. 

The kernel module is developed and tested on [AAarch/ARM64 Linux kernel](https://git.kernel.org/pub/scm/linux/kernel/git/arm64/linux.git/) version 5.19.0. A simple single-threaded test application that requests the service offered by the kernel module, and thus shows how the kernel module can be used is also included. The application takes 3 arguments: the period of the job it executes, the processing time of the job, the number of times of execution, and does the following:
1. Upon starting, the application register itself with the RMS module and pass admission control.
2. Read from `/proc/rms/status` to ensure that the registeration succeeded and its pid is listed.
   The list has the following format:
    ```
    <pid 1>: <period 1>, <processing time 1>
    <pid 2>: <period 2>, <processing time 2>
    ...
    <pid n>: <period n>, <processing time n>
    ```
3. Signal the RMS module it is ready to start by sending a YIELD message via `/proc/rms/status`.
4. Initiate a real-time loop and execute periodic dummy jobs that run for the proceesing time assigned as command argument. Each job is equivalent to one iteration of the real-time loop; after each job, the process yield and wait for the RMS module to wake it up for the next round of computation. Each iteration of the loop (i.e., each job) also prints how long the job took to wake up after the perious job, and how long the job took to complete, in the following format:
    ```
    wakeup: <wakeup_time>, process: <process_time>
    ```
5. Once all jobs are done, deregister itself via `/proc/rms/status`.

## Build and Installation
Compile the module and install.
```
$ cd rms
$ make
$ sudo insmod rms.ko
```
Show installed modules, including this module.
```
$ lsmod
$ modinfo rms.ko
```
Play with the module and the test application.
```
$ ./userapp [period_ms] [processing_time_ms] [num_of_execution]
```
Uninstall the module.
```
$ sudo rmmod rms
```
Remove the ouput files during compilation.
```
$ make clean
```

## Implementation
* The scheduler is developed on top of the Linux kernel scheduler, relying on its API to perform context switches.
* It manages the registered application with a list of `rms_task_struct` that contains a pointer to `task_struct` of a registered application and extra state needed for RT scheduling. 
    ```
    struct rms_task_struct {
        struct task_struct *task;
        struct timer_list wakeup_timer;
        struct list_head list;
        ....
        enum task_state state;
    }
    ```
* The slab allocator is used to improve the performance of object memory allocation in the kernel for `rms_task_struct`s. A cache of size `sizeof(struct rms_task_struct)` is set up for that and used by the REGISTRATION handler function to allocate new `rms_task_stuct` instances.
* A registered application will have 3 states indicated by the `state` member of associated `rms_task_struct`: `SLEEPING`, `READY` and `RUNNING`. 
* A kernel thread (the dispatching thread) that is responsible for triggering context switches as needed is created. It will sleep the rest of the time. There will be two cases in which a context switch will occur:
    1. after receiving a YIELD message from the application, and
    2. after the wakeup timer of the application expires.
* The YIELD handler sets the state of the calling application to `SLEEPING`, sets the wakeup timer to expire at the beginning of the next period, put the `task_struct` of the application to sleep as `TASK_UNINTERRUPTIBLE`, and wakes the dispatching thread up.
* The wakeup timer handler sets the state of the application to `READY` and wakes up the dispatching thread. 
* As soon as the dispatching thread wakes up, it finds the READY task with the hight priority in the task in the list, sets the new task's state to `RUNNING`, the currently running task's state to READY, respectively, and preempts the currently running task for the new task, if any.
* To implement admission control, fixed-point arithmetic is used for calculations, which would be done in floating-point one in userspace. 