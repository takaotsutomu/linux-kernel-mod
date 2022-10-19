#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "test_userapp.h"

#define MAX_STR_SIZE 255
#define RUNNING_TIME 10

int main(int argc, char *argv[])
{
	int pid;
	char command[MAX_STR_SIZE];
	time_t start_time;

	memset(command, 0, MAX_STR_SIZE);

	/* Register the process with its pid */
	pid = getpid();
	sprintf(command, "echo %d > /proc/usrt/status", pid);
	if (system(command)) {
		fprintf(stderr, "Command execution failed\n");
	}

	/* Run for some time specified as RUNNING_TIME */
	start_time = time(NULL);
	while (1) {
		if ((int)time(NULL) - start_time > RUNNING_TIME)
			break;
	}
	return 0;
}
