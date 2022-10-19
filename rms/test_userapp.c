#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <math.h>
#include "test_userapp.h"

#define MAX_STR_SIZE 255
#define RUNNING_TIME 10
#define MAX_READ_LEN 999


static int register_rms(int pid,
						char *period_ms,
						char *runtime_ms)
{
	char command[MAX_STR_SIZE];

	memset(command, 0, MAX_STR_SIZE);
	sprintf(command, "echo \"R, %d, %s, %s\" > /proc/rms/status",
			pid, period_ms, runtime_ms);
	if (system(command)) {
		fprintf(stderr, "System call failed\n");
		return 1;
	}
	return 0;
}

static int pid_in_list(int nr)
{
	FILE *rms;
	char rbuf[MAX_READ_LEN+1], *tok;
	int len, pid;

	rms = fopen("/proc/rms/status", "r");
	len = fread(rbuf, sizeof(char), MAX_READ_LEN, rms);
	rbuf[len] = '\0';
	fclose(rms);

	tok = strtok(rbuf, "\n");
	while (tok) {
		sscanf(tok, "%d", &pid);
		if (pid == nr)
			return 1;
		tok = strtok(NULL, "\n");
	}
	return 0;
}

static int yield_rms(int pid)
{
	char command[MAX_STR_SIZE];

	memset(command, 0, MAX_STR_SIZE);
	sprintf(command, "echo \"Y, %d\" > /proc/rms/status", pid);
	if (system(command)) {
		fprintf(stderr, "system call failed\n");
		return 1;
	}
	return 0;

}

static int deregister_rms(int pid)
{
	char command[MAX_STR_SIZE];

	memset(command, 0, MAX_STR_SIZE);
	sprintf(command, "echo \"D, %d\" > /proc/rms/status", pid);
	if (system(command)) {
		fprintf(stderr, "system call failed\n");
		return 1;
	}
	return 0;
}

static unsigned long timevalsub_ms(struct timeval *t0,
						      struct timeval *t1)
{
	struct timeval diff;

	timersub(t0, t1, &diff);
	return diff.tv_sec * 1000UL + diff.tv_usec / 1000UL;
}

static void do_job(unsigned long runtime_ms)
{
	struct timeval t0, t1;
	unsigned long diff_ms;

	gettimeofday(&t0, NULL);
	while (1) {
		gettimeofday(&t1, NULL);
		diff_ms =  timevalsub_ms(&t0, &t1);
		if (diff_ms > runtime_ms)
			break;
	}
}

/* 
 * Takes 3 arguments: 
 *
 * @runtime_ms: execution time of the job in msec
 * @period_ms: period of the job in msec
 * @cycle_nr: number of times the job should execute
 *           
 */
int main(int argc, char *argv[])
{
	struct timeval t0, t1, t2, diff1, diff2;
	unsigned long wakeup_time, process_time;
	unsigned long runtime_ms, period_ms;
	int cycle_nr, pid;

	if (argc != 4) {
		fprintf(stderr, "Invalid Arguments\n");
		return 1;
	}
	runtime_ms = strtoul(argv[1], NULL, 10);
	period_ms = strtoul(argv[2], NULL, 10);
	cycle_nr = atoi(argv[3]);

	/* Register the process with its pid */
	pid = getpid();
	if (register_rms(pid, argv[1], argv[2])) {
		fprintf(stderr, "Registeration failed\n");
		return 1;
	} 
	/* 
	 * Make sure the registration succeeded at the 
	 * RMS module. 
	 */ 
	if (!pid_in_list(pid)) {
		fprintf(stderr, "Registeration failed\n");
		return 1;
	}
	
	gettimeofday(&t0, NULL);
	/* 
	 * Everything is set. Yield at this point and let 
	 * the RMS module take care of scheduling.
	 */
	if (yield_rms(pid)) {
		fprintf(stderr, "Yield request failed\n");
		return 1;
	}

	/* This is the real-time loop */
	for (int i = 0; i < cycle_nr; i++) {
		/* Woken by the RMS for the job */
		gettimeofday(&t1, NULL);
		do_job(runtime_ms);
		gettimeofday(&t2, NULL);
		/* 
		 * The job is done. 
		 * Calculate the statistics and print them out.
		 */
		wakeup_time = timevalsub_ms(&t0, &t1);
		process_time = timevalsub_ms(&t1, &t2);
		fprintf(stdout, "%d wakeup: %lu, process: %lu\n",
				pid, wakeup_time, process_time);
		/* 
		 * Yield again to allow the RMS module to schedule
		 * the other tasks.
		 */
		if (yield_rms(pid)) {
			fprintf(stderr, "Yield request failed\n");
			break;
		}
	}

	/* Deregister the process with its pid */
	if (deregister_rms(pid)) {
		fprintf(stderr, "De-registeration failed\n");
		return 1;
	}

	return 0;
}