// SPDX-License-Identifier: GPL-2.0
/*
 * Workload to check whether the scheduler tick leaks into userspace on a
 * nohz_full CPU after an idle trip.
 *
 * Each iteration sleeps (the CPU goes idle; with idle=poll the tick is
 * restarted there unless the kernel stops it), then busy-loops in
 * userspace without syscalls. The busy window is delimited by
 * "BUSY_START"/"BUSY_END" trace_marker writes, so any tick
 * (hrtimer_expire_entry function=tick_nohz_handler) traced inside a
 * window has interrupted userspace.
 *
 * Usage: nohz_user_tick <cpu> [iterations] [sleep_us] [busy_ms]
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MARKER "/sys/kernel/tracing/trace_marker"

/* vDSO, no syscall as long as the clocksource supports it */
static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void busy_loop(long long ns)
{
	long long end = now_ns() + ns;

	while (now_ns() < end)
		;
}

static void marker(int fd, const char *s)
{
	if (write(fd, s, strlen(s)) < 0)
		perror("trace_marker");
}

int main(int argc, char **argv)
{
	int cpu;
	int iters = argc > 2 ? atoi(argv[2]) : 200;
	long sleep_us = argc > 3 ? atol(argv[3]) : 100;
	long busy_ms = argc > 4 ? atol(argv[4]) : 10;
	struct timespec req = {
		.tv_sec = sleep_us / 1000000,
		.tv_nsec = (sleep_us % 1000000) * 1000,
	};
	cpu_set_t set;
	int fd, i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <cpu> [iterations] [sleep_us] [busy_ms]\n", argv[0]);
		return 1;
	}
	cpu = atoi(argv[1]);

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) {
		perror("sched_setaffinity");
		return 1;
	}

	fd = open(MARKER, O_WRONLY);
	if (fd < 0) {
		perror(MARKER);
		return 1;
	}

	/* Let the CPU settle after the migration */
	busy_loop(100 * 1000000LL);

	for (i = 0; i < iters; i++) {
		clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
		marker(fd, "BUSY_START");
		busy_loop(busy_ms * 1000000LL);
		marker(fd, "BUSY_END");
	}

	close(fd);
	return 0;
}
