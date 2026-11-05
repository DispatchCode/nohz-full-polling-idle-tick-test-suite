// SPDX-License-Identifier: GPL-2.0
/*
 * Check that timer_list timers enqueued on a nohz_full CPU sitting in
 * polling idle (idle=poll) still fire on time.
 *
 * If the tick is stopped while polling, a timer is only programmed when
 * the idle loop re-evaluates the tick, and cpu_idle_poll() only returns on
 * need_resched(). The enqueue kicks the target CPU:
 *
 * mode=remote: add_timer_on() from the CPU loading the module. On x86 the
 *              kick reaches a polling CPU through
 *              call_function_single_prep_ipi(), which sets NEED_RESCHED.
 * mode=local:  a pinned hard hrtimer fires on the target CPU while it
 *              polls and calls add_timer_on() for that same CPU from
 *              hardirq, which takes the local self-kick path.
 *
 * second_ms=N arms a second timer N ms after the first one, right after
 * it. Only the first expiry gets programmed when the tick is stopped: the
 * second one depends on the tick being re-evaluated after the first fires.
 *
 * Usage (as root, from a housekeeping CPU):
 *   insmod nohz_poll_timer_test.ko cpu=<nohz_full cpu> mode=local [iters=20]
 *          [delay_ms=10] [second_ms=0] [settle_ms=50] [timeout_ms=2000]
 *   dmesg | tail
 *   rmmod nohz_poll_timer_test
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/tick.h>
#include <linux/timer.h>

static int cpu = -1;
module_param(cpu, int, 0444);
MODULE_PARM_DESC(cpu, "Target nohz_full CPU (required)");

static char *mode = "local";
module_param(mode, charp, 0444);
MODULE_PARM_DESC(mode, "local (enqueue from hardirq on target) or remote");

static int iters = 20;
module_param(iters, int, 0444);

static int delay_ms = 10;
module_param(delay_ms, int, 0444);
MODULE_PARM_DESC(delay_ms, "Timer expiry, relative to enqueue");

static int second_ms;
module_param(second_ms, int, 0444);
MODULE_PARM_DESC(second_ms, "If set, also arm a second timer this many ms after the first");

static int settle_ms = 50;
module_param(settle_ms, int, 0444);
MODULE_PARM_DESC(settle_ms, "Time for the target to go back to polling idle");

static int timeout_ms = 2000;
module_param(timeout_ms, int, 0444);
MODULE_PARM_DESC(timeout_ms, "Extra wait before declaring a timer lost");

struct test_timer {
	struct timer_list timer;
	struct completion fired;
	long late_jiffies;
};

static struct test_timer timers[2];
static int nr_timers;
static struct hrtimer arm_hrtimer;
static bool armed_in_idle;

/*
 * Lateness is measured against the programmed expiry, so that a delay
 * while arming (e.g. the vCPU preempted by the host) doesn't count.
 */
static void test_timer_fn(struct timer_list *t)
{
	struct test_timer *tt = container_of(t, struct test_timer, timer);

	tt->late_jiffies = (long)(jiffies - t->expires);
	complete(&tt->fired);
}

static void arm_test_timers(void)
{
	unsigned long now = jiffies;
	int i;

	for (i = 0; i < nr_timers; i++) {
		timers[i].timer.expires = now + msecs_to_jiffies(delay_ms + i * second_ms);
		add_timer_on(&timers[i].timer, cpu);
	}
}

/* Hardirq on the target CPU, normally interrupting polling idle */
static enum hrtimer_restart arm_hrtimer_fn(struct hrtimer *h)
{
	armed_in_idle = is_idle_task(current);
	arm_test_timers();
	return HRTIMER_NORESTART;
}

static void start_arm_hrtimer(void *unused)
{
	hrtimer_start(&arm_hrtimer, ms_to_ktime(settle_ms),
		      HRTIMER_MODE_REL_PINNED_HARD);
}

/* Make the target leave and re-enter idle, so that its tick gets stopped */
static void idle_cycle(void *unused)
{
}

static void cancel_all(void)
{
	int i;

	hrtimer_cancel(&arm_hrtimer);
	for (i = 0; i < ARRAY_SIZE(timers); i++)
		timer_delete_sync(&timers[i].timer);
}

static int __init nohz_poll_timer_test_init(void)
{
	bool local = !strcmp(mode, "local");
	s64 late_us, max_late_us = 0, sum_late_us = 0;
	int i, t, ok = 0, lost = 0, not_idle = 0;
	unsigned long wait;

	if (!local && strcmp(mode, "remote"))
		return -EINVAL;
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return -EINVAL;
	if (second_ms < 0)
		return -EINVAL;
	if (!tick_nohz_full_cpu(cpu))
		pr_warn("nohz_poll_timer_test: CPU %d is not nohz_full\n", cpu);
	if (!local && cpu == raw_smp_processor_id())
		pr_warn("nohz_poll_timer_test: loading from the target CPU, use taskset\n");

	nr_timers = second_ms ? 2 : 1;
	for (t = 0; t < ARRAY_SIZE(timers); t++) {
		timer_setup(&timers[t].timer, test_timer_fn, TIMER_PINNED);
		init_completion(&timers[t].fired);
	}
	hrtimer_setup(&arm_hrtimer, arm_hrtimer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_PINNED_HARD);

	wait = msecs_to_jiffies(settle_ms + delay_ms + second_ms + timeout_ms);

	for (i = 0; i < iters; i++) {
		for (t = 0; t < nr_timers; t++)
			reinit_completion(&timers[t].fired);

		if (local) {
			armed_in_idle = false;
			smp_call_function_single(cpu, start_arm_hrtimer, NULL, 1);
		} else {
			smp_call_function_single(cpu, idle_cycle, NULL, 1);
			msleep(settle_ms);
			arm_test_timers();
		}

		for (t = 0; t < nr_timers; t++) {
			if (!wait_for_completion_timeout(&timers[t].fired, wait)) {
				lost++;
				pr_info("nohz_poll_timer_test: iter %d timer %d: LOST (not fired after %d ms)\n",
					i, t, delay_ms + t * second_ms + timeout_ms);
				continue;
			}
			late_us = (s64)timers[t].late_jiffies * (USEC_PER_SEC / HZ);
			max_late_us = max(max_late_us, late_us);
			sum_late_us += late_us;
			ok++;
		}
		/* Lost timers must not fire into the next iteration */
		cancel_all();

		if (local && !armed_in_idle)
			not_idle++;
	}

	pr_info("nohz_poll_timer_test: cpu=%d mode=%s iters=%d delay_ms=%d timers=%d: fired=%d lost=%d avg_late_us=%lld max_late_us=%lld\n",
		cpu, mode, iters, delay_ms, nr_timers, ok, lost,
		ok ? sum_late_us / ok : 0, max_late_us);
	if (not_idle)
		pr_info("nohz_poll_timer_test: %d enqueues did not happen in idle, results less meaningful\n",
			not_idle);

	return 0;
}

static void __exit nohz_poll_timer_test_exit(void)
{
	cancel_all();
}

module_init(nohz_poll_timer_test_init);
module_exit(nohz_poll_timer_test_exit);
MODULE_DESCRIPTION("Timer enqueue on a nohz_full CPU in polling idle");
MODULE_LICENSE("GPL");
