#ifndef __RMS_H__
#define __RMS_H__

#define FILENAME       "status"
#define DIRECTORY      "rms"
#define REGISTERATION  'R'
#define YIELD		   'Y'
#define DEREGISTRATION 'D'

enum task_state { READY, RUNNING, SLEEPING };

/* Defined for fixed-point arithemric, where
 * in this kernel module the fractional part 
 * will take the least significant 14 bits 
 * and the integral part takes the rest, i.e,
 * the multiplier is going to be 16384.
 */
#define SHIFT_AMOUNT 14 /* 2^14 = 16384 */
#define SHIFT_MASK ((1 << SHIFT_AMOUNT) - 1)

#define set_task_state(tsk, state_value)        \
    smp_store_mb((tsk)->__state, (state_value))

#endif