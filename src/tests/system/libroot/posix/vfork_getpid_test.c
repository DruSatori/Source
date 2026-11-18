/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*!	Checks that a vfork() child sees its own process ID: getpid() must not
	return the parent's ID, and setpgid(getpid(), getpid()) must succeed.
*/


#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


int
main()
{
	pid_t parent = getpid();
	pid_t child = vfork();
	if (child < 0) {
		fprintf(stderr, "vfork() failed: %s\n", strerror(errno));
		return 1;
	}
	if (child == 0) {
		// only _exit() is allowed here; report via the exit status
		pid_t self = getpid();
		if (self == parent)
			_exit(2);
		if (setpgid(self, self) != 0)
			_exit(3);
		_exit(0);
	}

	int status;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
		fprintf(stderr, "waitpid() failed\n");
		return 1;
	}
	switch (WEXITSTATUS(status)) {
		case 0:
			printf("PASS: parent %d, vfork child %d, setpgid ok\n", parent,
				child);
			return 0;
		case 2:
			printf("FAIL: vfork child getpid() returned the parent's ID %d\n",
				parent);
			return 1;
		case 3:
			printf("FAIL: vfork child setpgid(getpid(), getpid()) failed\n");
			return 1;
	}
	printf("FAIL: child exit status %d\n", WEXITSTATUS(status));
	return 1;
}
