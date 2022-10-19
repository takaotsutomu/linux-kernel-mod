# Linux Usertime Kernel Module
## Overview
**This Linux kernel module measures the userspace CPU time of processes registered within the kernel module.** It keeps the `pid`s and CPU time values of each process in a Linux kernel linked list and updates the the values every 5 seconds with a kernel timer. The Two-Havles approach is used in handling the software timer interrputs. In this approach interrupt handling is dived in two parts: interrupt hander (the Top-Half) and worker thread (the Bottom-Half). In this kernel module, the Top-Half is `shedule_cput_upds()` whose sole purose is to wake up the Bottom-Half, i.e., to shedule the work function `update_cputimes()` with a workqueue, which allows one to schedule the execution of work functions at a later time. `update_cputimes()` traverses the link list and update the CPU time values of each regirstered process. When there are no registered processes, the work of updating the CPU times will not be sheduled. 

The kernel module is designed to support multiple processes (i.e., applications) to register simultaneously. The registration process is implemented as follows:
* At the initilaization of the kernel module, it create a directory entry --`/proc/urt`-- within the Proc filesystem .
* Inside the directory the kernel module creates a file entry --`/proc/urt/status`-- readable and writable by anyone.
* Upon start of a process, it will register itself by writing its `pid` to `/proc/urt/status`.
* When a process reads from `/proc/urt/status` the kernel module prints a list of all the registered `pid` in the system and the corresponding userspace CPU times in the following format. 
```
pid1: CPU time of pid1
pid2: CPU time of pid2
```

The kernel module is developed and tested on [AAarch/ARM64 Linux kernel](https://git.kernel.org/pub/scm/linux/kernel/git/arm64/linux.git/) version 5.19.0. A simple test application that requests the service offered by the kernel module, and thus shows how the kernel module can be used, is also included. 

## Build and Installation
Compile the module and install.
```
$ cd usertime
$ make
$ sudo insmod usertime.ko
```
Show installed modules, including this module.
```
$ lsmod
$ modinfo usertime.ko
```
Uninstall the module.
```
$ sudo rmmod usertime
```
Remove the ouput files during compilation.
```
$ make clean
```

## A note on locking (for myself)
Since v4.1.0, where `IRQF_DIABLED` (became no-op since v2.6.35) was removed, interrupt handlers (Hard IQR handlers; upper half of interupt handling) always run with all local interrupts disabled, i.e., as if `IRQF_DIABLED` is set as default. As a result,  `spin_lock_irq` or `spin_lock_irqsave` are not necessary for ensuring safty when data has to be shared between two Hard IRQ handlers: the vanilla `spin_lock()` will do. 

Linux used to support two types of interrupt handlers: the short and long ones. Short interrupt handlers were running with `IRQF_DIABLED` set (or `SA_INTERRUPT` in even older versions) and are supposed to finish their execution very quickly. Long interrupt handlers, on the other hand, were running with the other interrupts re-enabled (e.g. on x86, all local interrupts are disabled when jumping from a interrupt gate to a interupt handler, but Linux used to, by default, mask the same interrupt and re-enables the rest) because they would be doing a lot of work. This means that the long interrupt handlers can be interrupted by other interrupts, i.e., nested interrupts were possible. In modern versions of Linux, all interrupts are considered as the short one, and are running with all local interrupts disabled, i.e., nested interrrupts are not possible anymore. The part of interupt handling work that is too much to do in a interupt handler should be deferred and shedule later as the bottom half of the interrupt. 

For locking in this kernel module, `mutex_lock()` and `mutex_unlock()` are used because they allow more efficient usage of CPU resources. Linux mutex is a form of two-phase lock. When the lock is already held, `mutex_lock()` first spins while current lock holder is running because chances are the lock is about to be released (the first phase). If the lock is not aquired during the spin phase, it puts the task to sleep, relinguishing the CPU (the second phase). `mutex_lock()`/`mutex_unlock()` is a valid option because `usr_write()/usr_read()` and the work thread (kernel context) run in process context, and strict correctness is not required in the timer interrupt handler `schedule_cupt_upds()` (softriq; interrupt context).