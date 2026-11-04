// SPDX-License-Identifier: GPL-2.0
/*
 * Tick dependency checks for a nohz_full CPU whose tick gets stopped in
 * polling idle. Whatever needs the tick must still bring it back once
 * the CPU leaves idle.
 *
 * prof:  sleep/busy cycles with ITIMER_PROF armed. POSIX CPU timers are
 *        only checked from the tick, so the tick must restart whenever
 *        the task runs (TICK_DEP_BIT_POSIX_TIMER, restarted through the
 *        kick in __tick_nohz_task_switch()). Compares the SIGPROF count
 *        with the CPU time consumed.
 *          nohz_dep_test prof <cpu> [iters] [sleep_us] [busy_ms] [interval_ms]
 *
 * share: two CPU-bound tasks woken together on the CPU after it has been
 *        idle. TICK_DEP_BIT_SCHED must restart the tick so that they get
 *        preempted and share the CPU. Shares are in permille of run_ms.
 *          nohz_dep_test share <cpu> [idle_ms] [run_ms]
 *
 * Both print a "RESULT ..." line.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NSEC_PER_MSEC	1000000LL

static long long now_ns(clockid_t clk)
{
	struct timespec ts;

	clock_gettime(clk, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void busy_until(long long end)
{
	while (now_ns(CLOCK_MONOTONIC) < end)
		;
}

static void pin(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) {
		perror("sched_setaffinity");
		exit(1);
	}
}

static volatile sig_atomic_t nr_sigprof;

static void sigprof_handler(int sig)
{
	nr_sigprof++;
}

static int do_prof(int cpu, int iters, long sleep_us, long busy_ms,
		   long interval_ms)
{
	struct sigaction sa = {
		.sa_handler = sigprof_handler,
		.sa_flags = SA_RESTART,
	};
	struct itimerval it = {
		.it_interval = {
			.tv_sec = interval_ms / 1000,
			.tv_usec = (interval_ms % 1000) * 1000,
		},
	};
	struct timespec req = {
		.tv_sec = sleep_us / 1000000,
		.tv_nsec = (sleep_us % 1000000) * 1000,
	};
	long long cpu_start, cpu_ns;
	int i;

	pin(cpu);
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGPROF, &sa, NULL)) {
		perror("sigaction");
		return 1;
	}

	/* Let the CPU settle after the migration */
	busy_until(now_ns(CLOCK_MONOTONIC) + 100 * NSEC_PER_MSEC);

	it.it_value = it.it_interval;
	cpu_start = now_ns(CLOCK_PROCESS_CPUTIME_ID);
	if (setitimer(ITIMER_PROF, &it, NULL)) {
		perror("setitimer");
		return 1;
	}

	for (i = 0; i < iters; i++) {
		/* SIGPROF may interrupt the sleep, which is fine */
		nanosleep(&req, NULL);
		busy_until(now_ns(CLOCK_MONOTONIC) + busy_ms * NSEC_PER_MSEC);
	}

	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_PROF, &it, NULL);
	cpu_ns = now_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;

	printf("RESULT signals=%d expected=%lld cpu_ms=%lld\n", (int)nr_sigprof,
	       cpu_ns / (interval_ms * NSEC_PER_MSEC), cpu_ns / NSEC_PER_MSEC);
	return 0;
}

struct share_result {
	long long cpu_ns;
	long nivcsw;
};

static void share_child(int cpu, int start_fd, int res_fd)
{
	struct share_result r;
	struct rusage ru;
	long long end, cpu_start;
	long nivcsw_start;

	pin(cpu);

	/* Block here so that the CPU goes idle until the parent says go */
	if (read(start_fd, &end, sizeof(end)) != sizeof(end))
		_exit(1);

	cpu_start = now_ns(CLOCK_THREAD_CPUTIME_ID);
	getrusage(RUSAGE_SELF, &ru);
	nivcsw_start = ru.ru_nivcsw;

	busy_until(end);

	r.cpu_ns = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
	getrusage(RUSAGE_SELF, &ru);
	r.nivcsw = ru.ru_nivcsw - nivcsw_start;
	if (write(res_fd, &r, sizeof(r)) != sizeof(r))
		_exit(1);
	_exit(0);
}

static int do_share(int cpu, long idle_ms, long run_ms)
{
	struct share_result r[2];
	int start[2], res[2], i;
	long long end;
	pid_t pid[2];

	if (pipe(start) || pipe(res)) {
		perror("pipe");
		return 1;
	}

	for (i = 0; i < 2; i++) {
		pid[i] = fork();
		if (pid[i] < 0) {
			perror("fork");
			return 1;
		}
		if (!pid[i])
			share_child(cpu, start[0], res[1]);
	}

	/* Children are blocked on the target CPU, which is now idle */
	usleep(idle_ms * 1000);

	/* Both run until the same absolute deadline */
	end = now_ns(CLOCK_MONOTONIC) + run_ms * NSEC_PER_MSEC;
	for (i = 0; i < 2; i++) {
		if (write(start[1], &end, sizeof(end)) != sizeof(end)) {
			perror("write");
			return 1;
		}
	}

	for (i = 0; i < 2; i++) {
		if (read(res[0], &r[i], sizeof(r[i])) != sizeof(r[i])) {
			perror("read");
			return 1;
		}
	}
	for (i = 0; i < 2; i++)
		waitpid(pid[i], NULL, 0);

	printf("RESULT share0=%lld share1=%lld nivcsw0=%ld nivcsw1=%ld\n",
	       r[0].cpu_ns * 1000 / (run_ms * NSEC_PER_MSEC),
	       r[1].cpu_ns * 1000 / (run_ms * NSEC_PER_MSEC),
	       r[0].nivcsw, r[1].nivcsw);
	return 0;
}

int main(int argc, char **argv)
{
	int cpu;

	if (argc < 3) {
		fprintf(stderr,
			"usage: %s prof <cpu> [iters] [sleep_us] [busy_ms] [interval_ms]\n"
			"       %s share <cpu> [idle_ms] [run_ms]\n",
			argv[0], argv[0]);
		return 1;
	}
	cpu = atoi(argv[2]);

	if (!strcmp(argv[1], "prof"))
		return do_prof(cpu,
			       argc > 3 ? atoi(argv[3]) : 100,
			       argc > 4 ? atol(argv[4]) : 100,
			       argc > 5 ? atol(argv[5]) : 20,
			       argc > 6 ? atol(argv[6]) : 10);
	if (!strcmp(argv[1], "share"))
		return do_share(cpu,
				argc > 3 ? atol(argv[3]) : 200,
				argc > 4 ? atol(argv[4]) : 2000);

	fprintf(stderr, "unknown mode %s\n", argv[1]);
	return 1;
}
